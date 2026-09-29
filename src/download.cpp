#include "download.h"

#include <gtk/gtk.h>

#include "config.h"
#include "dialogs.h"
#include "proton.h"
#include "task_log.h"
#include "ui.h"
#include "util.h"
#include "window_mode.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace dload {

namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// env_config.yaml - the two-level YAML file behind the "Download Online" URLs
// ---------------------------------------------------------------------------

// Python: DEFAULT_ENV_CONFIG. The same URLs that are written to
// ~/wlm/env_config.yaml on the very first start, so users have an editable
// example instead of having to guess the format.
constexpr const char* DEFAULT_GE_RELEASES_API =
    "https://api.github.com/repos/GloriousEggroll/proton-ge-custom/releases";
constexpr const char* DEFAULT_GE_RELEASES_PAGE =
    "https://github.com/GloriousEggroll/proton-ge-custom/releases";
constexpr const char* DEFAULT_CACHYOS_RELEASES_API =
    "https://api.github.com/repos/CachyOS/proton-cachyos/releases";
constexpr const char* DEFAULT_CACHYOS_RELEASES_PAGE =
    "https://github.com/CachyOS/proton-cachyos/releases";

struct Urls {
    std::string releases_api;
    std::string releases_page;
};

struct EnvConfig {
    Urls proton_ge;
    Urls proton_cachyos;
};

EnvConfig default_env_config() {
    EnvConfig defaults;
    defaults.proton_ge.releases_api = DEFAULT_GE_RELEASES_API;
    defaults.proton_ge.releases_page = DEFAULT_GE_RELEASES_PAGE;
    defaults.proton_cachyos.releases_api = DEFAULT_CACHYOS_RELEASES_API;
    defaults.proton_cachyos.releases_page = DEFAULT_CACHYOS_RELEASES_PAGE;
    return defaults;
}

std::string trim(const std::string& s) {
    const size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

// The writer never quotes a URL, but a hand-edited file may - accept both so
// copying a quoted value from somewhere else keeps working.
std::string unquote(const std::string& value) {
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
        value.back() == value.front()) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

// Python: save_env_config_if_missing() - write env_config.yaml with the
// built-in defaults while the file does not exist yet, so the user immediately
// gets an example file they can edit. PyYAML is not available in this port, so
// the tiny writer below reproduces exactly the shape yaml.safe_dump() would
// have produced: section keys at indent 0, 'key: value' entries indented by 4.
void write_default_env_config() {
    std::ofstream out(cfg::env_config_file);
    if (!out) {
        std::fprintf(stderr, "[WLM] Error writing %s\n",
                     cfg::env_config_file.string().c_str());
        return;
    }

    const EnvConfig defaults = default_env_config();
    out << "# Wine Launch Manager - environment configuration\n"
        << "# URLs used by the \"Download Online\" feature to check for and fetch new\n"
        << "# Proton GE / Proton-CachyOS builds. Edit and save, then restart WLM (or\n"
        << "# reopen the Download Online dialog) to apply changes.\n"
        << "\n"
        << "proton_ge:\n"
        << "    releases_api: " << defaults.proton_ge.releases_api << "\n"
        << "    releases_page: " << defaults.proton_ge.releases_page << "\n"
        << "\n"
        << "proton_cachyos:\n"
        << "    releases_api: " << defaults.proton_cachyos.releases_api << "\n"
        << "    releases_page: " << defaults.proton_cachyos.releases_page << "\n";
}

// Python: load_env_config() - read the file and fill in the defaults for
// anything that is missing, empty or broken, so the callers always work with a
// complete set of URLs and never have to check for nullptr anywhere. Only the
// fixed 2-level shape this file uses is understood: section keys at indent 0
// ending with ':', 'key: value' entries indented below them, blank lines and
// '#' comments ignored.
EnvConfig load_env_config() {
    EnvConfig config = default_env_config();

    std::ifstream in(cfg::env_config_file);
    if (!in) return config;

    std::string section;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        const size_t indent = line.find_first_not_of(" \t");
        if (indent == std::string::npos) continue;  // blank line

        const std::string body = trim(line);
        if (body.empty() || body[0] == '#') continue;  // comment

        if (indent == 0) {
            // Section header such as "proton_ge:" - everything indented below
            // belongs to it until the next header shows up.
            if (body.back() == ':') {
                section = trim(body.substr(0, body.size() - 1));
            } else {
                section.clear();
            }
            continue;
        }

        const size_t colon = body.find(':');
        if (colon == std::string::npos || section.empty()) continue;

        const std::string key = trim(body.substr(0, colon));
        std::string value = trim(body.substr(colon + 1));

        // Drop an inline comment: the values are plain URLs, so a '#' that is
        // preceded by whitespace can only be a note the user added.
        const size_t hash = value.find(" #");
        if (hash != std::string::npos) value = trim(value.substr(0, hash));
        value = unquote(value);
        if (value.empty()) continue;

        if (section == "proton_ge") {
            if (key == "releases_api") config.proton_ge.releases_api = value;
            if (key == "releases_page") config.proton_ge.releases_page = value;
        } else if (section == "proton_cachyos") {
            if (key == "releases_api") config.proton_cachyos.releases_api = value;
            if (key == "releases_page") config.proton_cachyos.releases_page = value;
        }
    }
    return config;
}

// ---------------------------------------------------------------------------
// Running the system transfer tools
// ---------------------------------------------------------------------------

// The project has no networking library (no libcurl bindings, no sockets
// wrapper), so every HTTP request is delegated to the `curl` binary - or to
// `wget` when curl is not installed - the same way the Python version simply
// delegated to urllib. The command runs through /bin/sh with its stdout (and
// optionally stderr) connected to a pipe we read from, and the child is always
// reaped so no zombie is ever left behind.
class ShellPipe {
public:
    ShellPipe() = default;
    ShellPipe(const ShellPipe&) = delete;
    ShellPipe& operator=(const ShellPipe&) = delete;

    ~ShellPipe() { stop(); }

    // Starts `/bin/sh -c command`; false when fork/pipe/fdopen failed.
    bool start(const std::string& command, bool capture_stderr) {
        int out_fds[2];
        int err_fds[2];
        if (pipe(out_fds) != 0) return false;
        if (capture_stderr && pipe(err_fds) != 0) {
            close(out_fds[0]);
            close(out_fds[1]);
            return false;
        }

        // Children must not inherit the AppImage's bundled LD_LIBRARY_PATH
        // (see util::clean_env()).
        util::EnvBlock env(util::clean_env());

        const pid_t pid = fork();
        if (pid < 0) {
            close(out_fds[0]);
            close(out_fds[1]);
            if (capture_stderr) {
                close(err_fds[0]);
                close(err_fds[1]);
            }
            return false;
        }

        if (pid == 0) {
            close(out_fds[0]);
            dup2(out_fds[1], STDOUT_FILENO);
            close(out_fds[1]);
            if (capture_stderr) {
                close(err_fds[0]);
                dup2(err_fds[1], STDERR_FILENO);
                close(err_fds[1]);
            }

            // sh -c so the command may use quoting, redirection and pipes.
            std::vector<char*> argv;
            argv.push_back(const_cast<char*>("sh"));
            argv.push_back(const_cast<char*>("-c"));
            argv.push_back(const_cast<char*>(command.c_str()));
            argv.push_back(nullptr);
            execvpe(argv[0], argv.data(), env.data());
            _exit(127);
        }

        close(out_fds[1]);
        if (capture_stderr) close(err_fds[1]);

        out_ = fdopen(out_fds[0], "rb");
        if (capture_stderr) err_ = fdopen(err_fds[0], "rb");
        if (!out_) close(out_fds[0]);
        if (capture_stderr && !err_) close(err_fds[0]);

        pid_ = pid;
        if (!out_ || (capture_stderr && !err_)) {
            stop();  // also reaps the child, nothing can leak
            return false;
        }
        return true;
    }

    FILE* out() const { return out_; }

    // Everything the child wrote to stdout / stderr. Both tools are started in
    // silent mode, so stderr only ever carries a short error message and can
    // never fill its pipe while we are still blocking on stdout.
    std::string stdout_text() { return read_stream(out_); }
    std::string stderr_text() { return trim(read_stream(err_)); }

    // Closes the pipes and returns the child's exit code (-1 on a signal).
    int wait() {
        close_streams();
        return reap();
    }

    // Cancels a still running command (the task window's Cancel button): drop
    // the pipe first so the child cannot block on a full buffer, then SIGTERM.
    void stop() {
        close_streams();
        if (pid_ > 0) kill(pid_, SIGTERM);
        reap();
    }

private:
    static std::string read_stream(FILE* file) {
        if (!file) return "";
        std::string text;
        char buffer[8192];
        size_t got = 0;
        while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
            text.append(buffer, got);
        }
        return text;
    }

    void close_streams() {
        if (out_) {
            fclose(out_);
            out_ = nullptr;
        }
        if (err_) {
            fclose(err_);
            err_ = nullptr;
        }
    }

    int reap() {
        if (pid_ < 0) return -1;
        int status = 0;
        while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        return -1;
    }

    pid_t pid_ = -1;
    FILE* out_ = nullptr;
    FILE* err_ = nullptr;
};

bool have_command(const std::string& name) {
    ShellPipe probe;
    if (!probe.start("command -v " + util::shell_quote(name) + " >/dev/null 2>&1", false)) {
        return false;
    }
    return probe.wait() == 0;
}

// Probing costs a fork, so remember the answer for the whole session.
bool curl_available() {
    static const bool available = have_command("curl");
    return available;
}

bool wget_available() {
    static const bool available = have_command("wget");
    return available;
}

// GET url with the GitHub API headers and a 15 second timeout - Python's
// urllib.request.urlopen() call. The transfer tool's stderr is captured
// separately so a failed request reports its real reason (no DNS, HTTP 403
// rate limit, ...) instead of an empty body, and any failure throws
// std::runtime_error because the Python version raised as well - the dialog
// shows that message to the user.
std::string http_get(const std::string& url) {
    std::string command;
    if (curl_available()) {
        command = "curl -fsSL --max-time 15"
                  " -H " +
                  util::shell_quote("User-Agent: wine-launcher-manager") + " -H " +
                  util::shell_quote("Accept: application/vnd.github+json") + " " +
                  util::shell_quote(url);
    } else if (wget_available()) {
        command = "wget -qO- --timeout=15"
                  " --user-agent=" +
                  util::shell_quote("wine-launcher-manager") + " --header=" +
                  util::shell_quote("Accept: application/vnd.github+json") + " " +
                  util::shell_quote(url);
    } else {
        throw std::runtime_error("Neither curl nor wget is installed - cannot reach GitHub.");
    }

    ShellPipe child;
    if (!child.start(command, true)) {
        throw std::runtime_error("Could not run the download tool (curl/wget).");
    }

    const std::string body = child.stdout_text();
    const std::string error_text = child.stderr_text();
    const int status = child.wait();
    if (status != 0) {
        std::string message = error_text;
        if (message.empty()) {
            message = "network request failed (exit code " + std::to_string(status) + ")";
        }
        throw std::runtime_error(message);
    }
    return body;
}

// ---------------------------------------------------------------------------
// Release list
// ---------------------------------------------------------------------------

// One downloadable build - exactly the fields Python keeps in its release
// dicts: tag/name/published_at/asset_name/browser_download_url/size.
struct Release {
    std::string tag;
    std::string name;
    std::string published;  // published_at limited to the date (YYYY-MM-DD)
    std::string asset_name;
    std::string download_url;
    long long size = 0;
};

const char* const ARCHIVE_SUFFIXES[] = {".tar.gz", ".tar.xz", ".tgz", ".zip"};

bool ends_with(const std::string& text, const std::string& suffix) {
    if (text.size() < suffix.size()) return false;
    return text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_archive_asset(const std::string& asset_name) {
    const std::string lower = util::lower(asset_name);
    for (const char* suffix : ARCHIVE_SUFFIXES) {
        if (ends_with(lower, suffix)) return true;
    }
    return false;
}

// Python: _strip_archive_suffix() - drop .tar.gz/.tar.xz/.tgz/.zip from a file
// name.
std::string archive_suffix(const std::string& asset_name) {
    const std::string lower = util::lower(asset_name);
    for (const char* suffix : ARCHIVE_SUFFIXES) {
        if (ends_with(lower, suffix)) return suffix;
    }
    return "";
}

// Python: variant_label() - one CachyOS release can ship several builds, the
// file name tells them apart.
std::string variant_label(const std::string& asset_name) {
    const std::string lower = util::lower(asset_name);
    if (lower.find("native") != std::string::npos) return "Native";
    if (lower.find("slr") != std::string::npos) return "SLR";
    return "Standalone";
}

// CachyOS publishes one asset per CPU target for every variant, e.g.
// proton-cachyos-11.0-20260703-slr-x86_64.tar.xz,
// …-slr-x86_64_v3.tar.xz and …-slr-arm64.tar.xz. All three rows have the same
// release name, so the variant column alone ("SLR") could not tell them apart
// and the user had no way to see which architecture a build was for. The arch
// is now appended to the variant text; the most specific token wins, and an
// unknown name keeps the bare variant label.
std::string arch_label(const std::string& asset_name) {
    const std::string lower = util::lower(asset_name);

    // A micro-architecture level ("_v3", "-v2", ...) is more specific than the
    // plain architecture, so it has to be looked for first - every vN name
    // also contains the bare "x86_64".
    static const char* const level_separators[] = {"_v", "-v", "_", "-"};
    static const char* const levels[] = {"2", "3", "4", "5"};
    for (const char* level : levels) {
        for (const char* separator : level_separators) {
            if (lower.find(std::string("x86_64") + separator + level) != std::string::npos ||
                lower.find(std::string("x86-64") + separator + level) != std::string::npos ||
                lower.find(std::string("amd64") + separator + level) != std::string::npos) {
                return std::string("amd64 v") + level;
            }
        }
    }

    if (lower.find("x86_64") != std::string::npos) return "amd64";
    if (lower.find("x86-64") != std::string::npos) return "amd64";
    if (lower.find("amd64") != std::string::npos) return "amd64";
    if (lower.find("aarch64") != std::string::npos) return "arm64";
    if (lower.find("arm64") != std::string::npos) return "arm64";
    return "";
}

// What the Variant column and the confirmation dialog show: the variant plus
// the architecture, e.g. "SLR (amd64 v3)".
std::string variant_text(const std::string& asset_name, bool show_variant) {
    if (!show_variant) return "";
    const std::string variant = variant_label(asset_name);
    const std::string arch = arch_label(asset_name);
    return arch.empty() ? variant : variant + " (" + arch + ")";
}

// Turns the GitHub API body into the row list. Proton GE publishes a single
// .tar.gz per release (the .sha512sum checksums are skipped by that test), so
// one release becomes one row; a Proton-CachyOS release often ships several
// variants at once (SLR & Native & ...), so there every archive asset becomes
// its own row - exactly like the Python loops do.
std::vector<Release> parse_release_list(const std::string& body, bool every_archive_asset) {
    json data = json::parse(body, nullptr, false);
    if (data.is_discarded() || !data.is_array()) {
        // Usually an HTML error page - show the first bit instead of a blank
        // "could not parse" message.
        std::string message = trim(body);
        if (message.size() > 300) message = message.substr(0, 300) + "...";
        if (message.empty()) message = "empty response";
        throw std::runtime_error("GitHub returned an unexpected response: " + message);
    }

    std::vector<Release> releases;
    for (const json& rel : data) {
        const std::string tag = json_str(rel, "tag_name");
        const std::string published_at = json_str(rel, "published_at");
        const std::string name = json_str(rel, "name");

        Release entry;
        entry.tag = tag;
        entry.name = name.empty() ? tag : name;
        entry.published = published_at.substr(0, 10);

        for (const json& asset : json_arr(rel, "assets")) {
            const std::string asset_name = json_str(asset, "name");
            const bool wanted = every_archive_asset ? is_archive_asset(asset_name)
                                                    : ends_with(util::lower(asset_name), ".tar.gz");
            if (!wanted) continue;

            entry.asset_name = asset_name;
            entry.download_url = json_str(asset, "browser_download_url");
            entry.size = json_int(asset, "size");
            releases.push_back(entry);
            if (!every_archive_asset) break;  // Proton GE: only the first .tar.gz counts
        }
    }
    return releases;
}

// Python: fetch_protonge_releases() - blocking network call, always run it on
// a background thread. Throws std::runtime_error on failure (no internet,
// GitHub rate limit, ...) so the dialog can show the reason.
std::vector<Release> fetch_protonge_releases(const std::string& api_url, int limit = 100) {
    const std::string url = api_url + "?per_page=" + std::to_string(limit);
    return parse_release_list(http_get(url), false);
}

// Python: fetch_protoncachyos_releases()
std::vector<Release> fetch_protoncachyos_releases(const std::string& api_url, int limit = 20) {
    const std::string url = api_url + "?per_page=" + std::to_string(limit);
    return parse_release_list(http_get(url), true);
}

// ---------------------------------------------------------------------------
// Download + extract workers
// ---------------------------------------------------------------------------

// Thrown by the worker when the task window's Cancel button was pressed -
// reported as a plain cancellation instead of an error dialog.
struct CancelledError : std::runtime_error {
    CancelledError() : std::runtime_error("Download cancelled.") {}
};

// Python: tempfile.mkstemp(prefix=..., suffix=...). mkstemp() insists on a
// template ending in XXXXXX, so the archive suffix is re-attached with a rename
// afterwards - proton::extract_archive_to_dir() uses the file name as the
// wrapper folder name when the tarball has no single root folder, and libarchive
// keeps sniffing the real format anyway.
fs::path make_temp_archive(const std::string& prefix, const std::string& suffix) {
    const std::string tmpl = (fs::temp_directory_path() / (prefix + "XXXXXX")).string();
    std::vector<char> buffer(tmpl.begin(), tmpl.end());
    buffer.push_back('\0');

    const int fd = mkstemp(buffer.data());
    if (fd < 0) {
        throw std::runtime_error("Could not create a temporary file: " +
                                 std::string(std::strerror(errno)));
    }
    close(fd);

    const fs::path plain(buffer.data());
    const fs::path with_suffix(plain.string() + suffix);
    std::error_code ec;
    fs::rename(plain, with_suffix, ec);
    if (ec) {
        fs::remove(plain, ec);
        throw std::runtime_error("Could not prepare the temporary file: " + ec.message());
    }
    return with_suffix;
}

// The asset body goes to stdout; stderr is captured separately so a transfer
// error page can never end up inside the archive.
std::string download_command(const std::string& url) {
    if (curl_available()) {
        return "curl -fsSL --speed-limit 1024 --speed-time 60 -H " +
               util::shell_quote("User-Agent: wine-launcher-manager") + " " +
               util::shell_quote(url);
    }
    if (wget_available()) {
        return "wget -qO- --timeout=60 --user-agent=" +
               util::shell_quote("wine-launcher-manager") + " " + util::shell_quote(url);
    }
    throw std::runtime_error("Neither curl nor wget is installed - cannot download releases.");
}

// Streams the asset into tmp_path in 64 KiB chunks, reporting progress through
// the task (bytes written against the advertised asset size). Returns the
// number of bytes written.
//
// Nothing is logged while the transfer runs: the log window is for the
// milestones ("Downloading ...", "Download complete ...", the result), and the
// per-chunk movement is the progress bar's job. The bar gets the percentage and
// the live rate, refreshed on a fixed short interval so the figure is stable
// enough to read rather than flickering on every 64 KiB block.
long long stream_download(const std::string& url, const fs::path& tmp_path,
                          const tasklog::TaskPtr& task, long long total_size) {
    ShellPipe child;
    if (!child.start(download_command(url), true)) {
        throw std::runtime_error("Could not run the download tool (curl/wget).");
    }

    std::ofstream file(tmp_path, std::ios::binary | std::ios::trunc);
    if (!file) {
        child.stop();
        throw std::runtime_error("Could not open the temporary file for writing.");
    }

    // How often the percentage and the speed are refreshed, and how much earlier
    // data the speed is averaged over.
    const auto refresh_interval = std::chrono::milliseconds(500);
    const double speed_window_seconds = 1.5;

    long long written = 0;
    auto window_start = std::chrono::steady_clock::now();
    long long window_start_bytes = 0;
    auto last_refresh = window_start;
    // Smoothed rate: each new sample is blended into the previous one, so the
    // label settles instead of alternating between two values.
    double speed = 0.0;
    std::vector<char> buffer(64 * 1024);

    while (true) {
        if (task->cancel_requested()) {
            file.close();
            child.stop();
            throw CancelledError();
        }

        const size_t got = fread(buffer.data(), 1, buffer.size(), child.out());
        if (got == 0) break;

        file.write(buffer.data(), static_cast<std::streamsize>(got));
        if (!file) {
            const std::string reason = std::strerror(errno);
            file.close();
            child.stop();
            throw std::runtime_error("Could not write to the temporary file: " + reason);
        }
        written += static_cast<long long>(got);

        const auto now = std::chrono::steady_clock::now();
        if (now - last_refresh < refresh_interval) continue;
        last_refresh = now;

        if (total_size > 0) {
            task->set_progress(100.0 * static_cast<double>(written) /
                               static_cast<double>(total_size));
        }

        // Bytes per second over the trailing window, smoothed.
        const double elapsed = std::chrono::duration<double>(now - window_start).count();
        if (elapsed >= speed_window_seconds) {
            const double instant =
                static_cast<double>(written - window_start_bytes) / elapsed;
            speed = speed > 0.0 ? (speed * 0.4 + instant * 0.6) : instant;
            window_start = now;
            window_start_bytes = written;
        }
        task->set_speed(speed);
    }

    file.close();
    const std::string error_text = child.stderr_text();
    const int status = child.wait();
    if (status != 0) {
        std::string message = error_text;
        if (message.empty()) {
            message = "download failed (exit code " + std::to_string(status) + ")";
        }
        throw std::runtime_error(message);
    }
    return written;
}

// Status line + outcome dialog handed back to the GTK main thread: a worker
// thread may never touch a widget itself. The popup is the only reliable way
// to tell the user a finished download, because the task log window is
// non-modal and may be hidden behind the release list at that moment.
struct FinalReport {
    std::string status_text;
    std::string status_kind = "info";
    std::string dialog_title;  // empty -> no dialog
    std::string dialog_message;
    bool dialog_is_error = false;  // false -> the "installed" confirmation
};

void free_task_ptr(gpointer data) { delete static_cast<tasklog::TaskPtr*>(data); }

// Back on the main thread: the worker is done, so Cancel is pointless and
// Close / the WM's X may really close the log window.
gboolean mark_task_finished_idle(gpointer data) {
    static_cast<tasklog::TaskPtr*>(data)->get()->mark_finished();
    return G_SOURCE_REMOVE;
}

gboolean apply_final_report(gpointer data) {
    std::unique_ptr<FinalReport> report(static_cast<FinalReport*>(data));
    ui::set_status(report->status_text, report->status_kind);
    if (!report->dialog_title.empty()) {
        if (report->dialog_is_error) {
            dialogs::show_error(report->dialog_title, report->dialog_message, ui::window);
        } else {
            dialogs::show_info(report->dialog_title, report->dialog_message, ui::window);
        }
    }
    return G_SOURCE_REMOVE;
}

void report_final(FinalReport report) {
    g_idle_add(apply_final_report, new FinalReport(std::move(report)));
}

// Python: download_and_install_protonge_worker() /
// download_and_install_protoncachyos_worker(). Runs on the download's background
// thread: stream the asset to a temp file, extract it into target_dir with the
// same helper the manual "Extract ... Archive..." menu uses, then delete the
// archive again. Every step is logged through the task; the status line and the
// outcome dialog are bounced back to the main thread.
void download_worker(const tasklog::TaskPtr& task, const Release& release,
                     const fs::path& target_dir, const std::string& flavor,
                     const std::string& display, const std::string& tmp_prefix) {
    fs::path tmp_path;
    try {
        task->append_line("Downloading " + release.asset_name + " (" +
                          util::human_size(static_cast<double>(release.size)) + ")...");
        tmp_path = make_temp_archive(tmp_prefix, archive_suffix(release.asset_name));

        const long long written =
            stream_download(release.download_url, tmp_path, task, release.size);
        if (release.size > 0) task->set_progress(100);

        task->append_line("");
        task->append_line("Download complete (" + util::human_size(static_cast<double>(written)) +
                          "). Extracting to " + target_dir.string() + "...");

        // Nothing left to count against: the percentage and the rate go away and
        // the bar pulses for the rest of the install.
        task->set_speed(0);
        task->set_busy();

        // Extraction runs on the same worker thread, so Cancel keeps working
        // here too. It logs nothing: the bar simply keeps pulsing (no
        // percentage is knowable for an unpack), and the callback's only job is
        // to notice the cancel and abort - proton:: then removes the folders it
        // had already created.
        const bool extracted = proton::extract_archive_to_dir(
            tmp_path, target_dir, [&](long long) { return !task->cancel_requested(); });
        if (!extracted) throw CancelledError();

        task->append_line("");
        const std::string success = flavor + " " + display + " installed successfully.";
        task->append_line(success);
        report_final({success, "success", flavor + " " + display + " installed",
                      "Installed into:\n" + target_dir.string(), false});
    } catch (const CancelledError&) {
        task->append_line("");
        task->append_line("Download cancelled.");
        report_final({flavor + " download cancelled.", "warning", "", ""});
    } catch (const std::exception& e) {
        const std::string error = e.what();
        task->append_line("");
        task->append_line("ERROR: " + error);
        report_final({flavor + " download failed: " + error, "error", "Download Failed",
                      "Failed to download/install " + flavor + ":\n" + error, true});
    } catch (...) {
        task->append_line("");
        task->append_line("ERROR: unknown error");
        report_final({flavor + " download failed.", "error", "Download Failed",
                      "Failed to download/install " + flavor + ":\nUnknown error.", true});
    }

    if (!tmp_path.empty()) {
        // Python's "finally: tmp_path.unlink()" - never leave a half written
        // archive in the temp folder, whatever happened above.
        std::error_code ec;
        fs::remove(tmp_path, ec);
    }
}

// ---------------------------------------------------------------------------
// The download dialog
// ---------------------------------------------------------------------------

enum { COL_BUILD, COL_VARIANT, COL_DATE, COL_SIZE, COL_COUNT };

struct DialogOptions {
    std::string title;       // window title
    std::string heading;     // line above the status label
    std::string flavor;      // "ProtonGE" / "Proton-CachyOS"
    std::string page_url;    // "Open Releases Page" target
    std::string count_unit;  // "release(s)" / "build(s)"
    std::string tmp_prefix;  // temp archive name prefix
    fs::path target_dir;     // where the archive gets extracted
    bool show_variant = false;
    // Download button label / status line used once a download has been
    // started and its progress moved into the log window.
    std::string log_button_label = "Download & Install";
    std::string log_button_status = "Downloading - progress is in the download log window.";
    std::function<std::vector<Release>()> fetch;
    std::function<std::string(const Release&)> message_name;
};

// Owned by a shared_ptr that is attached to the window, so a background fetch
// that finishes after the dialog was closed only has to look at one flag.
struct DialogCtx : std::enable_shared_from_this<DialogCtx> {
    DialogOptions opts;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    bool destroyed = false;  // set by the window's "destroy" handler

    GtkWidget* window = nullptr;
    GtkWidget* status_label = nullptr;
    GtkWidget* refresh_btn = nullptr;
    GtkWidget* download_btn = nullptr;
    GtkWidget* show_log_btn = nullptr;
    GtkWidget* tree = nullptr;
    GtkListStore* store = nullptr;  // released together with the tree view
    std::vector<Release> releases;

    // The download task log this dialog started. It outlives a closed log
    // window, so "Show Download Log" can raise it again; it is kept out of the
    // tasklog busy registry (see open_download_dialog) so a download never
    // blocks - or gets hidden by - a prefix backup/restore.
    std::shared_ptr<tasklog::Task> task;
};

struct FetchResult {
    std::shared_ptr<DialogCtx> ctx;
    std::vector<Release> releases;
    std::string error;
};

void add_tree_column(GtkWidget* tree, const char* title, int model_col, int width,
                     gboolean expand, const char* tooltip = nullptr) {
    GtkCellRenderer* renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn* column =
        gtk_tree_view_column_new_with_attributes(title, renderer, "text", model_col, NULL);
    gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(column, winmode::px(width));
    gtk_tree_view_column_set_expand(column, expand);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

    // GTK3 has no gtk_tree_view_column_set_tooltip_text(); the header button is
    // what shows the tooltip. It only exists once the column is packed.
    if (tooltip != nullptr) {
        GtkWidget* header = gtk_tree_view_column_get_button(column);
        if (header != nullptr) gtk_widget_set_tooltip_text(header, tooltip);
    }
}

void populate(DialogCtx* ctx, const std::vector<Release>& releases) {
    gtk_list_store_clear(ctx->store);
    ctx->releases = releases;

    for (const Release& release : releases) {
        const std::string variant = variant_text(release.asset_name, ctx->opts.show_variant);
        GtkTreeIter iter;
        gtk_list_store_append(ctx->store, &iter);
        gtk_list_store_set(ctx->store, &iter, COL_BUILD, release.name.c_str(), COL_VARIANT,
                           variant.c_str(), COL_DATE, release.published.c_str(), COL_SIZE,
                           util::human_size(static_cast<double>(release.size)).c_str(), -1);
    }
}

gboolean apply_fetch_result(gpointer data) {
    std::unique_ptr<FetchResult> result(static_cast<FetchResult*>(data));
    const std::shared_ptr<DialogCtx>& ctx = result->ctx;
    if (!ctx->alive->load()) return G_SOURCE_REMOVE;  // dialog closed meanwhile

    gtk_widget_set_sensitive(ctx->refresh_btn, TRUE);

    if (!result->error.empty()) {
        gtk_label_set_text(GTK_LABEL(ctx->status_label),
                           ("Failed to load releases: " + result->error).c_str());
        dialogs::show_error(
            "Error",
            "Failed to fetch the " + ctx->opts.flavor +
                " release list from GitHub:\n" + result->error +
                "\n\nCheck your internet connection and try Refresh again.",
            GTK_WINDOW(ctx->window));
        return G_SOURCE_REMOVE;
    }

    populate(ctx.get(), result->releases);
    gtk_label_set_text(GTK_LABEL(ctx->status_label),
                       (std::to_string(result->releases.size()) + " " + ctx->opts.count_unit +
                        " loaded.")
                           .c_str());
    return G_SOURCE_REMOVE;
}

// Python: load_releases()'s worker thread - the GitHub call can block for the
// whole 15 s timeout, so it never runs on the main thread; the dialog only
// shows "Fetching releases..." and disables Refresh until the answer arrives.
void start_fetch(const std::shared_ptr<DialogCtx>& ctx) {
    gtk_widget_set_sensitive(ctx->refresh_btn, FALSE);
    gtk_label_set_text(GTK_LABEL(ctx->status_label), "Fetching releases...");

    auto* result = new FetchResult();
    result->ctx = ctx;
    const DialogOptions opts = ctx->opts;  // the fetch function travels with the thread

    std::thread([opts, result]() {
        try {
            result->releases = opts.fetch();
        } catch (const std::exception& e) {
            result->error = e.what();
        } catch (...) {
            result->error = "unknown error";
        }
        g_idle_add(apply_fetch_result, result);
    }).detach();
}

std::optional<Release> selected_release(DialogCtx* ctx) {
    GtkTreeSelection* selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(ctx->tree));
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(selection, nullptr, &iter)) return std::nullopt;

    GtkTreePath* path = gtk_tree_model_get_path(GTK_TREE_MODEL(ctx->store), &iter);
    if (!path) return std::nullopt;
    const gint* indices = gtk_tree_path_get_indices(path);
    const int row = indices ? indices[0] : -1;
    gtk_tree_path_free(path);

    if (row < 0 || row >= static_cast<int>(ctx->releases.size())) return std::nullopt;
    return ctx->releases[row];
}

void on_window_destroy(GtkWidget*, gpointer data) {
    auto* ctx = static_cast<DialogCtx*>(data);
    ctx->alive->store(false);
    ctx->destroyed = true;
}

// Both the "_Close" button (GTK_RESPONSE_CLOSE) and the WM's X button
// (GTK_RESPONSE_DELETE_EVENT) close this window. GtkDialog only auto-destroys
// itself for DELETE_EVENT, and the response has to be handled explicitly now
// that there is no gtk_dialog_run() behind it.
gboolean on_window_response(GtkWidget* window, gint response, gpointer) {
    if (response == GTK_RESPONSE_CLOSE || response == GTK_RESPONSE_DELETE_EVENT) {
        gtk_widget_destroy(window);
    }
    return TRUE;  // handled either way: never fall through to the default
}

void on_refresh_clicked(GtkButton*, gpointer data) {
    auto* ctx = static_cast<DialogCtx*>(data);
    try {
        start_fetch(ctx->shared_from_this());
    } catch (const std::exception& e) {
        ui::set_status(std::string("Error refreshing releases: ") + e.what(), "error");
    } catch (...) {
        ui::set_status("Error refreshing releases.", "error");
    }
}

void on_open_page_clicked(GtkButton*, gpointer data) {
    auto* ctx = static_cast<DialogCtx*>(data);
    try {
        int err = 0;
        if (!util::open_path(ctx->opts.page_url, &err)) {
            dialogs::show_error("Error", "Failed to open the releases page:\n" + ctx->opts.page_url,
                                GTK_WINDOW(ctx->window));
            ui::set_status("Could not open the releases page.", "error");
        }
    } catch (const std::exception& e) {
        ui::set_status(std::string("Could not open the releases page: ") + e.what(), "error");
    }
}

// Raises the download log window again after the user closed it. Closing the
// log only hides it (the worker keeps running either way), so this is what
// makes "close the log window, then carry on in this window" work.
void on_show_log_clicked(GtkButton*, gpointer data) {
    auto* ctx = static_cast<DialogCtx*>(data);
    try {
        if (ctx->task && ctx->task->window_alive()) {
            ctx->task->show();
            return;
        }
        dialogs::show_info("Download Log", "The download log window is already closed.",
                           GTK_WINDOW(ctx->window));
    } catch (const std::exception& e) {
        ui::set_status(std::string("Could not open the download log: ") + e.what(), "error");
    }
}

void on_download_clicked(GtkButton*, gpointer data) {
    auto* ctx = static_cast<DialogCtx*>(data);
    try {
        const std::optional<Release> selected = selected_release(ctx);
        if (!selected) {
            dialogs::show_info("Info", "Select a release from the list first.",
                               GTK_WINDOW(ctx->window));
            return;
        }
        // Copy everything now: the confirmation dialog below runs a nested main
        // loop, and a refresh completing during it would replace the list.
        const Release release = *selected;
        const std::string display = ctx->opts.message_name(release);

        std::string question = "Download and install " + ctx->opts.flavor + " " + display;
        const std::string variant = variant_text(release.asset_name, ctx->opts.show_variant);
        if (!variant.empty()) question += " (" + variant + ")";
        question += "?\n\nFile: " + release.asset_name +
                    "\nSize: " + util::human_size(static_cast<double>(release.size)) +
                    "\n\nIt will be extracted into:\n" + ctx->opts.target_dir.string();
        if (!dialogs::ask_yes_no("Download & Install " + ctx->opts.flavor, question,
                                 GTK_WINDOW(ctx->window))) {
            return;
        }

        const std::string log_title = "Download " + ctx->opts.flavor + " - " + display;
        const fs::path target_dir = ctx->opts.target_dir;
        const std::string flavor = ctx->opts.flavor;
        const std::string tmp_prefix = ctx->opts.tmp_prefix;

        // The task log window takes over from here: the worker streams its
        // append_line()/set_progress() updates into it while GTK stays free.
        // It is transient for the MAIN window, not for this release list, so
        // closing either one never takes the other away - and closing the log
        // while it runs only hides it, "Show Download Log" brings it back.
        ctx->task = tasklog::open(
            log_title, ui::window, true,
            "Hide this window. The download keeps running - reopen it with "
            "\"Show Download Log\" in the release list.",
            "Stop the download process currently running?\n\n"
            "The download is stopped and the temporary archive is deleted. A build that was "
            "already being extracted may be left incomplete - delete its folder and download it "
            "again in that case.");
        tasklog::TaskPtr task = ctx->task;
        if (ctx->opts.show_variant) {
            task->append_line("Release: " + release.name + " (" + release.tag + ") - " +
                              release.asset_name);
        } else {
            task->append_line("Release: " + release.name + " (" + release.tag + ")");
        }
        task->append_line("");

        // A download runs on its own thread instead of tasklog::run(): that
        // registry is the "one prefix backup/restore at a time" slot the
        // Prefix Configuration Manager guards, and a download must neither
        // block a backup nor be replaceable by one. mark_finished() is bounced
        // through an idle so the widget is touched on the main thread, exactly
        // like tasklog::run() does it.
        std::thread([release, target_dir, flavor, display, tmp_prefix, task]() {
            try {
                download_worker(task, release, target_dir, flavor, display, tmp_prefix);
            } catch (const std::exception& e) {
                task->append_line(std::string("[launcher] error: ") + e.what());
            } catch (...) {
                task->append_line("[launcher] error: unknown error");
            }
            // Higher priority than the outcome popup's idle, so the log window
            // is already marked finished when the "installed" dialog appears.
            g_idle_add_full(G_PRIORITY_DEFAULT, mark_task_finished_idle,
                            new tasklog::TaskPtr(task), free_task_ptr);
        }).detach();

        // From now on this list is only a launcher: the download runs in the log
        // window, which can be closed freely and raised again with the button
        // next to it.
        gtk_widget_set_sensitive(ctx->show_log_btn, TRUE);
        gtk_button_set_label(GTK_BUTTON(ctx->download_btn),
                             ctx->opts.log_button_label.c_str());
        gtk_label_set_text(GTK_LABEL(ctx->status_label),
                           ctx->opts.log_button_status.c_str());
    } catch (const std::exception& e) {
        ui::set_status(std::string("Download failed to start: ") + e.what(), "error");
        dialogs::show_error("Error", std::string("Failed to start the download:\n") + e.what(),
                            GTK_WINDOW(ctx->window));
    } catch (...) {
        ui::set_status("Download failed to start.", "error");
        dialogs::show_error("Error", "Failed to start the download.", GTK_WINDOW(ctx->window));
    }
}

// Python: open_protonge_download_dialog() / open_protoncachyos_download_dialog()
// - a window listing the newest GitHub releases with a Download & Install
// button, so nobody has to download a tarball from the browser and run
// "Extract ... Archive..." by hand. Same shape as the Prefix Configuration
// Manager: list on top, action buttons below.
void open_download_dialog(const DialogOptions& opts, GtkWindow* parent) {
    auto ctx = std::make_shared<DialogCtx>();
    ctx->opts = opts;

    // NOT modal, and not run through gtk_dialog_run()'s nested loop: the
    // download log is an independent top-level window, and a modal grab here
    // used to freeze it completely - Cancel and Close did nothing until the
    // release list had been closed first. Non-modal also keeps this window's
    // Refresh usable while a download runs.
    ctx->window = gtk_dialog_new_with_buttons(
        opts.title.c_str(), parent ? parent : ui::window,
        static_cast<GtkDialogFlags>(GTK_DIALOG_DESTROY_WITH_PARENT), "_Close",
        GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_resizable(GTK_WINDOW(ctx->window), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(ctx->window), winmode::px(720), winmode::px(560));

    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(ctx->window));
    gtk_container_set_border_width(GTK_CONTAINER(content), winmode::px(15));
    gtk_box_set_spacing(GTK_BOX(content), winmode::px(8));

    // ---- heading + Refresh ----
    GtkWidget* top_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, winmode::px(8));
    gtk_box_pack_start(GTK_BOX(content), top_bar, FALSE, FALSE, 0);

    GtkWidget* heading = gtk_label_new(opts.heading.c_str());
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
    gtk_box_pack_start(GTK_BOX(top_bar), heading, TRUE, TRUE, 0);

    ctx->refresh_btn = gtk_button_new_with_label("Refresh");
    gtk_widget_set_size_request(ctx->refresh_btn, winmode::px(110), -1);
    gtk_box_pack_start(GTK_BOX(top_bar), ctx->refresh_btn, FALSE, FALSE, 0);
    g_signal_connect(ctx->refresh_btn, "clicked", G_CALLBACK(on_refresh_clicked), ctx.get());

    ctx->status_label = gtk_label_new("Fetching releases...");
    gtk_label_set_xalign(GTK_LABEL(ctx->status_label), 0.0f);
    gtk_style_context_add_class(gtk_widget_get_style_context(ctx->status_label), "dim-label");
    gtk_box_pack_start(GTK_BOX(content), ctx->status_label, FALSE, FALSE, 0);

    // ---- release list ----
    ctx->store = gtk_list_store_new(COL_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                    G_TYPE_STRING);
    ctx->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ctx->store));
    g_object_unref(ctx->store);  // the view owns the model from here on

    GtkTreeSelection* selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(ctx->tree));
    gtk_tree_selection_set_mode(selection, GTK_SELECTION_BROWSE);

    add_tree_column(ctx->tree, "Build", COL_BUILD, 220, TRUE);
    // Wide enough for "Standalone (amd64 v3)": the variant column carries the
    // CPU architecture of the asset too, because one CachyOS release publishes
    // an archive per target under the same release name - see variant_text().
    if (opts.show_variant) {
        add_tree_column(ctx->tree, "Variant", COL_VARIANT, 150, FALSE,
                        "Build variant and CPU architecture of the file, e.g. "
                        "\"SLR (amd64 v3)\" or \"Native (arm64)\".");
    }
    add_tree_column(ctx->tree, "Published", COL_DATE, 100, FALSE);
    add_tree_column(ctx->tree, "Size", COL_SIZE, 80, FALSE);

    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), ctx->tree);
    gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 0);

    // ---- action buttons ----
    GtkWidget* btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, winmode::px(6));
    gtk_widget_set_halign(btn_row, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(content), btn_row, FALSE, FALSE, 0);

    GtkWidget* download_btn = gtk_button_new_with_label("Download & Install");
    GtkWidget* page_btn = gtk_button_new_with_label("Open Releases Page");
    gtk_box_pack_start(GTK_BOX(btn_row), download_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btn_row), page_btn, FALSE, FALSE, 0);
    g_signal_connect(download_btn, "clicked", G_CALLBACK(on_download_clicked), ctx.get());
    g_signal_connect(page_btn, "clicked", G_CALLBACK(on_open_page_clicked), ctx.get());
    ctx->download_btn = download_btn;

    // "Show Download Log" only becomes usable once a download has been started;
    // it re-raises the log window the user closed.
    ctx->show_log_btn = gtk_button_new_with_label("Show Download Log");
    gtk_widget_set_size_request(ctx->show_log_btn, winmode::px(150), -1);
    gtk_widget_set_tooltip_text(ctx->show_log_btn,
                                "Bring back the download progress window after closing it");
    gtk_widget_set_sensitive(ctx->show_log_btn, FALSE);
    gtk_box_pack_start(GTK_BOX(btn_row), ctx->show_log_btn, FALSE, FALSE, 0);
    g_signal_connect(ctx->show_log_btn, "clicked", G_CALLBACK(on_show_log_clicked), ctx.get());

    // The context must outlive this function: the window is no longer run
    // through a nested main loop, so the callbacks below keep using it after
    // open_download_dialog() has returned. It is owned by the window object and
    // released with it. A download it started is unaffected - the worker thread
    // and the log window's own poll timer both hold the Task, and the log
    // window is transient for the main window, so it keeps running (and stays
    // cancellable) after this list is closed.
    g_object_set_data_full(G_OBJECT(ctx->window), "dload-ctx", new std::shared_ptr<DialogCtx>(ctx),
                           [](gpointer p) { delete static_cast<std::shared_ptr<DialogCtx>*>(p); });
    g_signal_connect(ctx->window, "destroy", G_CALLBACK(on_window_destroy), ctx.get());

    // The "_Close" button and the WM's X both destroy this window.
    g_signal_connect(ctx->window, "response", G_CALLBACK(on_window_response), ctx.get());

    gtk_widget_show_all(ctx->window);
    start_fetch(ctx);
}

}  // namespace

// Writes ~/wlm/env_config.yaml with the default URLs when it does not exist
// yet (Python: save_env_config_if_missing(), called on import).
void ensure_env_config() {
    std::error_code ec;
    if (fs::exists(cfg::env_config_file, ec)) return;

    // Python created the ~/wlm folders at import time before saving; do the
    // same here so the first start cannot fail just because the folder is gone.
    const fs::path parent = cfg::env_config_file.parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);

    write_default_env_config();
}

void open_protonge_download_dialog(GtkWindow* parent) {
    try {
        const EnvConfig env = load_env_config();

        DialogOptions opts;
        opts.title = "Download ProtonGE";
        opts.heading = "Latest ProtonGE releases (GloriousEggroll/proton-ge-custom):";
        opts.flavor = "ProtonGE";
        opts.page_url = env.proton_ge.releases_page;
        opts.count_unit = "release(s)";
        opts.tmp_prefix = "wlm_protonge_";
        opts.target_dir = cfg::protonge_dir;
        opts.show_variant = false;
        // Proton GE ships one .tar.gz per release, so the download button keeps
        // its label and the status line just points at the new log window.
        opts.log_button_label = "Download & Install";
        opts.log_button_status = "Downloading - progress is in the download log window.";
        opts.fetch = [api = env.proton_ge.releases_api]() {
            return fetch_protonge_releases(api);
        };
        opts.message_name = [](const Release& release) { return release.tag; };

        open_download_dialog(opts, parent);
    } catch (const std::exception& e) {
        ui::set_status(std::string("ProtonGE download error: ") + e.what(), "error");
        dialogs::show_error("Error", std::string("Failed to open the ProtonGE download window:\n") +
                                         e.what(),
                            parent ? parent : ui::window);
    } catch (...) {
        ui::set_status("ProtonGE download error.", "error");
        dialogs::show_error("Error", "Failed to open the ProtonGE download window.",
                            parent ? parent : ui::window);
    }
}

void open_protoncachyos_download_dialog(GtkWindow* parent) {
    try {
        const EnvConfig env = load_env_config();

        DialogOptions opts;
        opts.title = "Download Proton-CachyOS";
        opts.heading = "Latest Proton-CachyOS builds (CachyOS/proton-cachyos):";
        opts.flavor = "Proton-CachyOS";
        opts.page_url = env.proton_cachyos.releases_page;
        opts.count_unit = "build(s)";
        opts.tmp_prefix = "wlm_protoncachyos_";
        opts.target_dir = cfg::protoncachyos_dir;
        // One CachyOS release often carries several variants (SLR/Native/...),
        // so every archive asset gets its own row and column.
        opts.show_variant = true;
        // Several builds per release, so after one download starts this window
        // is a launcher: pick the next build while the log window shows what the
        // running one is doing.
        opts.log_button_label = "Download & Install Another";
        opts.log_button_status = "Downloading - pick another build or close this window.";
        opts.fetch = [api = env.proton_cachyos.releases_api]() {
            return fetch_protoncachyos_releases(api);
        };
        opts.message_name = [](const Release& release) { return release.name; };

        open_download_dialog(opts, parent);
    } catch (const std::exception& e) {
        ui::set_status(std::string("Proton-CachyOS download error: ") + e.what(), "error");
        dialogs::show_error("Error",
                            std::string("Failed to open the Proton-CachyOS download window:\n") +
                                e.what(),
                            parent ? parent : ui::window);
    } catch (...) {
        ui::set_status("Proton-CachyOS download error.", "error");
        dialogs::show_error("Error", "Failed to open the Proton-CachyOS download window.",
                            parent ? parent : ui::window);
    }
}

}  // namespace dload
