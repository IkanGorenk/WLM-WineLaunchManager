#include "backup.h"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "config.h"
#include "dialogs.h"
#include "proton.h"
#include "scripts.h"
#include "task_log.h"
#include "ui.h"
#include "util.h"
#include "window_mode.h"

namespace backup {

namespace {

namespace fs = std::filesystem;

// ---- tiny string / time helpers ----

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string trim(const std::string& s) {
    size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string join(const std::vector<std::string>& names, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i != 0) out += sep;
        out += names[i];
    }
    return out;
}

// Local time formatted with strftime (Python: datetime.now().isoformat() for
// created_at, and the "%Y%m%d_%H%M%S" suffix of the default backup name).
std::string format_now(const char* fmt) {
    std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
    localtime_r(&now, &tm_buf);
    char buffer[64];
    if (std::strftime(buffer, sizeof(buffer), fmt, &tm_buf) == 0) return "";
    return std::string(buffer);
}

// An archive entry path made safe relative to its destination root: absolute
// paths and ".." components are dropped, so a crafted backup can never write
// outside the folder the user chose (same idea as proton.cpp).
fs::path sanitize_entry_path(const std::string& raw) {
    fs::path out;
    for (const auto& part : fs::path(raw)) {
        if (part.is_absolute()) continue;
        std::string s = part.string();
        if (s.empty() || s == "." || s == "..") continue;
        out /= s;
    }
    return out;
}

std::string archive_error(struct archive* a, const std::string& fallback) {
    const char* msg = archive_error_string(a);
    return msg ? std::string(msg) : fallback;
}

// Python: TaskCancelledError - thrown while archiving/extracting so the work
// stops the moment the user presses Cancel in the log window instead of
// running to the end.
struct TaskCancelledError : std::runtime_error {
    TaskCancelledError() : std::runtime_error("Cancelled by user.") {}
};

// ---- feedback posted from worker threads ----

// GTK widgets may only be touched from the main thread, so a worker never
// calls ui::set_status()/dialogs::show_error() itself: it hands the message
// over through an idle callback (Python: root.after(0, ...)).
struct Feedback {
    std::string status;
    std::string kind = "info";
    bool has_error = false;
    std::string error_title;
    std::string error_message;
    bool refresh_list = false;
};

gboolean apply_feedback(gpointer data) {
    auto* fb = static_cast<Feedback*>(data);
    if (!fb->status.empty()) ui::set_status(fb->status, fb->kind);
    if (fb->has_error) dialogs::show_error(fb->error_title, fb->error_message);
    if (fb->refresh_list) ui::update_script_list();
    delete fb;
    return G_SOURCE_REMOVE;
}

void post(Feedback fb) { g_idle_add(apply_feedback, new Feedback(std::move(fb))); }

void post_status(const std::string& text, const std::string& kind, bool refresh_list = false) {
    Feedback fb;
    fb.status = text;
    fb.kind = kind;
    fb.refresh_list = refresh_list;
    post(std::move(fb));
}

// Python: safe_status_config(...) + messagebox.showerror(...) after a failure.
void post_failure(const std::string& status, const std::string& title, const std::string& message) {
    Feedback fb;
    fb.status = status;
    fb.kind = "error";
    fb.has_error = true;
    fb.error_title = title;
    fb.error_message = message;
    post(std::move(fb));
}

// ---- "only one backup/restore at a time" ----

// dialogs.h has no warning flavour - Python uses messagebox.showwarning here.
void show_warning(const std::string& title, const std::string& message, GtkWindow* parent) {
    GtkWidget* dialog =
        gtk_message_dialog_new(parent ? parent : ui::window, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                               GTK_BUTTONS_OK, "%s", title.c_str());
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", message.c_str());
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

// Python prefix_task_is_busy(): only one prefix backup/restore may run at a
// time; its log window is where the running one is watched or cancelled.
bool warn_if_busy(GtkWindow* parent) {
    if (!tasklog::is_busy()) return false;
    show_warning("Backup/Restore Busy",
                 "A backup/restore is already running (" + tasklog::busy_label() + ").\n\n"
                 "Only one prefix/game backup or restore can run at a time. Use \"Show Logs\" "
                 "in the Prefix Configuration Manager to check its progress or cancel it first.",
                 parent);
    return true;
}

// ---- throttled per-file reporting (shared by both workers) ----

// Python's counters/last_report inside the tarfile progress_filter and inside
// extract_members: one log line + one progress update at most every 0.2s (but
// always for the first 3 files), so a big archive does not flood the log.
struct Progress {
    tasklog::TaskPtr task;
    // The archive being written; never archived into itself (see add_to_archive).
    std::optional<fs::path> dest;
    long long total_bytes = 0;
    long long done_bytes = 0;
    long long count = 0;
    std::chrono::steady_clock::time_point last_report;

    void report_backup(const std::string& name) {
        ++count;
        if (!due()) return;
        task->append_line("[" + right_aligned() + " files, " +
                          util::human_size(static_cast<double>(done_bytes)) + "] " + name);
        update_percent();
    }

    void report_restore(const std::string& relative) {
        ++count;
        if (!due()) return;
        task->append_line("[" + right_aligned() + "] " + relative);
        update_percent();
    }

private:
    bool due() {
        auto now = std::chrono::steady_clock::now();
        if (count <= 3 || now - last_report > std::chrono::milliseconds(200)) {
            last_report = now;
            return true;
        }
        return false;
    }

    std::string right_aligned() const {
        std::string n = std::to_string(count);
        if (n.size() < 6) n.insert(0, 6 - n.size(), ' ');
        return n;
    }

    void update_percent() {
        if (total_bytes <= 0) return;
        double percent = 100.0 * static_cast<double>(std::min(done_bytes, total_bytes)) /
                         static_cast<double>(total_bytes);
        task->set_progress(percent);
    }
};

// ---- libarchive RAII wrappers (modelled on proton.cpp) ----

class WriteArchive {
public:
    explicit WriteArchive(const fs::path& dest) {
        writer_ = archive_write_new();
        archive_write_add_filter_gzip(writer_);
        // PAX (Python's tarfile default): wine prefix paths are routinely
        // deeper than plain ustar's 100 character limit.
        archive_write_set_format_pax_restricted(writer_);
        if (archive_write_open_filename(writer_, dest.c_str()) != ARCHIVE_OK) {
            std::string msg = archive_error(writer_, "cannot create the archive");
            archive_write_free(writer_);
            writer_ = nullptr;
            throw std::runtime_error(msg);
        }
    }

    ~WriteArchive() {
        if (!writer_) return;
        archive_write_close(writer_);
        archive_write_free(writer_);
    }

    struct archive* get() const { return writer_; }

    // Called on the happy path so a failing flush (disk full) is reported
    // instead of being swallowed by the destructor.
    void finish() {
        if (!writer_) return;
        int rc = archive_write_close(writer_);
        std::string msg =
            rc == ARCHIVE_OK ? "" : archive_error(writer_, "failed to finish the archive");
        archive_write_free(writer_);
        writer_ = nullptr;
        if (!msg.empty()) throw std::runtime_error(msg);
    }

    WriteArchive(const WriteArchive&) = delete;
    WriteArchive& operator=(const WriteArchive&) = delete;

private:
    struct archive* writer_ = nullptr;
};

class ReadArchive {
public:
    explicit ReadArchive(const fs::path& src) {
        reader_ = archive_read_new();
        archive_read_support_filter_all(reader_);
        archive_read_support_format_all(reader_);
        if (archive_read_open_filename(reader_, src.c_str(), 10240) != ARCHIVE_OK) {
            std::string msg = archive_error(reader_, "cannot open the archive");
            archive_read_free(reader_);
            reader_ = nullptr;
            throw std::runtime_error(msg);
        }
    }

    ~ReadArchive() {
        if (!reader_) return;
        archive_read_close(reader_);
        archive_read_free(reader_);
    }

    struct archive* get() const { return reader_; }

    ReadArchive(const ReadArchive&) = delete;
    ReadArchive& operator=(const ReadArchive&) = delete;

private:
    struct archive* reader_ = nullptr;
};

class DiskWriter {
public:
    DiskWriter() {
        writer_ = archive_write_disk_new();
        archive_write_disk_set_options(writer_, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                                                 ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_FFLAGS |
                                                 ARCHIVE_EXTRACT_SECURE_SYMLINKS |
                                                 ARCHIVE_EXTRACT_SECURE_NODOTDOT);
    }

    ~DiskWriter() {
        if (writer_) archive_write_free(writer_);
    }

    struct archive* get() const { return writer_; }

    DiskWriter(const DiskWriter&) = delete;
    DiskWriter& operator=(const DiskWriter&) = delete;

private:
    struct archive* writer_ = nullptr;
};

// ---- gathering what has to be backed up ----

// Two paths are "the same file" when the OS says so (same inode / device), not
// when the strings match: the archive is named by the user and the tree spells
// it differently all the time.
bool same_file(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    const bool same_device = fs::equivalent(a, b, ec);
    if (!ec) return same_device;
    // equivalent() fails when one side does not exist (the archive is created
    // after the check, or vanished), so fall back to a resolved path compare.
    return util::resolve_path(a) == util::resolve_path(b);
}

bool is_within_resolved(const fs::path& child, const fs::path& parent) {
    const fs::path c = util::resolve_path(child);
    const fs::path p = util::resolve_path(parent);
    auto cit = c.begin();
    auto pit = p.begin();
    for (; pit != p.end(); ++cit, ++pit) {
        if (cit == c.end() || *cit != *pit) return false;
    }
    return true;
}


// Counts the bytes under one folder. `task` may be null (nothing to report to);
// when it is given, the running total is logged every ~0.5 s and Cancel is
// honoured - without that, scanning a multi-GB prefix looks exactly like a
// frozen launcher and Cancel does nothing. `files` counts every file seen so far
// across the whole walk, not just this folder.
void accumulate_directory(const fs::path& dir, long long* total, long long* files,
                          const tasklog::TaskPtr& task,
                          const std::optional<fs::path>& exclude = std::nullopt) {
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) return;

    auto last_report = std::chrono::steady_clock::now();

    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) return;

        if (task) {
            if (task->cancel_requested()) throw TaskCancelledError();
            const auto now = std::chrono::steady_clock::now();
            if (now - last_report >= std::chrono::milliseconds(500)) {
                last_report = now;
                task->append_line("  " + util::human_size(static_cast<double>(*total)) + " in " +
                                  std::to_string(*files) + " files so far...");
            }
        }

        // symlink_status, NOT directory_entry::is_directory(): the latter
        // FOLLOWS symlinks, and a Wine prefix has drive_c/dosdevices/z: -> /
        // (a symlink to the filesystem root) plus ~/Music, ~/Documents, ... -
        // following those recursed out of the prefix and walked the entire
        // disk, so the backup never finished and its "total size" was the size
        // of the whole machine. Symlinks are counted as the tiny entries they
        // are, exactly like the archiving walk below (and like tar).
        std::error_code tec;
        const fs::file_status child = fs::symlink_status(it->path(), tec);

        // Never count the archive this run is writing: it is inside the tree
        // when the user saves the backup into the prefix folder, and reading it
        // here would fold its growing size back into the total.
        if (exclude && same_file(it->path(), *exclude)) continue;

        if (!tec && fs::is_directory(child)) {
            accumulate_directory(it->path(), total, files, task, exclude);
        } else if (!tec && fs::is_regular_file(child)) {
            std::error_code sec;
            unsigned long long size = it->file_size(sec);
            if (!sec) *total += static_cast<long long>(size);
            ++*files;
        }
    }
}

// Python: compute_total_size() - the byte total the progress bar is a
// percentage of. Per-file errors (broken symlink, vanished file) are ignored
// on purpose: one bad file must not fail the counting, that file is simply
// tried again while it gets archived.
//
// The walk itself can take a while on a big prefix, so it logs its running
// total and honours Cancel (see accumulate_directory).
long long compute_total_size(const std::vector<fs::path>& paths,
                             const tasklog::TaskPtr& task = nullptr,
                             const std::optional<fs::path>& exclude = std::nullopt) {
    long long total = 0;
    long long files = 0;
    for (const fs::path& path : paths) {
        if (path.empty()) continue;
        if (task && task->cancel_requested()) throw TaskCancelledError();
        std::error_code ec;
        if (fs::is_regular_file(path, ec)) {
            if (exclude && same_file(path, *exclude)) continue;
            std::error_code sec;
            unsigned long long size = fs::file_size(path, sec);
            if (!sec) total += static_cast<long long>(size);
        } else if (fs::is_directory(path, ec)) {
            // The archive being written must never be counted as input, or it
            // feeds its own growing size into the total.
            if (exclude && is_within_resolved(*exclude, util::resolve_path(path))) {
                accumulate_directory(path, &total, &files, task, *exclude);
                continue;
            }
            accumulate_directory(path, &total, &files, task, std::nullopt);
        }
    }
    return total;
}

// One game that uses the prefix, as Python's gather_prefix_backup_info()
// describes it: where its folder is, whether that folder already sits inside
// the prefix (installed by a Windows installer straight into drive_c, so it is
// archived with the prefix and must NOT be archived a second time), and the
// runner settings that have to survive the round trip.
struct GameBackupInfo {
    std::string script_name;
    fs::path folder_path;      // empty when the script has no "cd" line
    std::string exe_path;      // "" when the script has no .exe line
    std::string relative_exe;  // exe_path relative to folder_path ("" = not possible)
    bool inside_prefix = false;
    std::string relative_to_prefix;
    std::string launch_options;
    std::string comment;
    bool missing = true;       // folder is gone from disk - reported BEFORE backup starts
    fs::path icon_path;
    bool has_icon = false;
};

// Python: gather_prefix_backup_info(entry).
std::vector<GameBackupInfo> gather_prefix_backup_info(const prefix::PrefixEntry& entry) {
    json runner_cfg = cfg::load_runner_config();
    fs::path prefix_resolved = util::resolve_path(fs::path(entry.prefix_path));

    std::vector<GameBackupInfo> games_info;
    for (const std::string& script_name : entry.games) {
        GameBackupInfo info;
        info.script_name = script_name;

        fs::path script_path = cfg::bashlaunch_dir / (script_name + ".sh");
        std::optional<std::string> folder = scripts::extract_folder_path_from_script(script_path);
        std::optional<std::string> exe = scripts::extract_exe_path_from_script(script_path);
        if (folder) info.folder_path = *folder;
        if (exe) info.exe_path = *exe;

        std::error_code ec;
        bool folder_exists = !info.folder_path.empty() && fs::is_directory(info.folder_path, ec);
        if (folder_exists && !info.exe_path.empty()) {
            // The restore rebuilds the exe address from the new folder, but
            // only when it really is below that folder (Python: relative_to()).
            fs::path folder_resolved = util::resolve_path(info.folder_path);
            fs::path exe_resolved = util::resolve_path(info.exe_path);
            if (util::is_within(exe_resolved, folder_resolved)) {
                info.relative_exe = exe_resolved.lexically_relative(folder_resolved).string();
            }
        }
        if (folder_exists) {
            fs::path folder_resolved = util::resolve_path(info.folder_path);
            if (util::is_within(folder_resolved, prefix_resolved)) {
                info.inside_prefix = true;
                info.relative_to_prefix = folder_resolved.lexically_relative(prefix_resolved).string();
            }
        }
        info.missing = !folder_exists;

        const json& game_cfg = json_obj(runner_cfg, script_name);
        info.launch_options = json_str(game_cfg, "launch_options");
        info.comment = json_str(game_cfg, "comment");

        info.icon_path = cfg::icon_dir / (script_name + ".png");
        info.has_icon = fs::is_regular_file(info.icon_path, ec);

        games_info.push_back(std::move(info));
    }
    return games_info;
}

// An empty string becomes JSON null, so the manifest records "no such thing"
// instead of an empty path that would later be taken literally.
json string_or_null(const std::string& value) {
    return value.empty() ? json(nullptr) : json(value);
}

// Python's manifest dict - run_restore_worker() rebuilds the prefix, the game
// scripts and runner_config.json from it on another machine.
json build_manifest(const prefix::PrefixEntry& entry,
                    const std::vector<GameBackupInfo>& games_info) {
    json manifest = json::object();
    manifest["format_version"] = 2;
    manifest["created_at"] = format_now("%Y-%m-%dT%H:%M:%S");
    manifest["runner"] = entry.runner;
    manifest["prefix_code"] = entry.prefix_code;

    json games = json::array();
    for (const GameBackupInfo& g : games_info) {
        json item = json::object();
        item["script_name"] = g.script_name;
        item["relative_exe"] = string_or_null(g.relative_exe);
        item["exe_path"] = string_or_null(g.exe_path);
        item["launch_options"] = g.launch_options;
        item["comment"] = g.comment;
        item["inside_prefix"] = g.inside_prefix;
        item["relative_to_prefix"] = string_or_null(g.relative_to_prefix);
        item["has_icon"] = g.has_icon;
        games.push_back(item);
    }
    manifest["games"] = games;
    return manifest;
}

// ---- writing the backup archive ----

unsigned perms_of(const fs::file_status& st) {
    return static_cast<unsigned>(st.permissions()) & 07777u;
}

std::time_t mtime_of(const fs::path& p) {
    std::error_code ec;
    fs::file_time_type ft = fs::last_write_time(p, ec);
    if (ec) return std::time(nullptr);
    // file_time_type's clock has no direct conversion to system_clock before
    // C++20 - go through "now" on both sides to stay within a second.
    auto sys_now = std::chrono::system_clock::now();
    auto file_now = fs::file_time_type::clock::now();
    auto when =
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(ft - file_now + sys_now);
    return std::chrono::system_clock::to_time_t(when);
}

struct archive_entry* make_entry(const std::string& pathname, la_int64_t size, mode_t filetype,
                                 unsigned perm, std::time_t mtime) {
    struct archive_entry* entry = archive_entry_new();
    archive_entry_set_pathname(entry, pathname.c_str());
    archive_entry_set_size(entry, size);
    archive_entry_set_filetype(entry, filetype);
    archive_entry_set_perm(entry, perm);
    archive_entry_set_mtime(entry, mtime, 0);
    return entry;
}

void write_header_or_throw(struct archive* writer, struct archive_entry* entry,
                           const std::string& pathname) {
    int rc = archive_write_header(writer, entry);
    archive_entry_free(entry);
    if (rc != ARCHIVE_OK) {
        throw std::runtime_error(archive_error(writer, "failed to write " + pathname));
    }
}

void add_regular_file(struct archive* writer, const fs::path& disk, const std::string& arcname,
                      Progress* progress) {
    std::error_code ec;
    std::uintmax_t size = fs::file_size(disk, ec);
    if (ec) throw fs::filesystem_error("file size", disk, ec);
    fs::file_status st = fs::symlink_status(disk, ec);
    if (ec) throw fs::filesystem_error("stat", disk, ec);

    write_header_or_throw(writer, make_entry(arcname, static_cast<la_int64_t>(size), AE_IFREG,
                                             perms_of(st), mtime_of(disk)),
                          arcname);

    // Counted and reported before the payload is copied - that is the moment
    // Python's tarfile progress_filter runs as well.
    progress->done_bytes += static_cast<long long>(size);
    progress->report_backup(arcname);

    std::ifstream in(disk, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + disk.string());
    std::vector<char> buffer(64 * 1024);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        std::streamsize got = in.gcount();
        if (got <= 0) break;
        if (progress->task->cancel_requested()) throw TaskCancelledError();
        if (archive_write_data(writer, buffer.data(), static_cast<size_t>(got)) < 0) {
            throw std::runtime_error(archive_error(writer, "failed to write " + arcname));
        }
    }
}

void add_symlink_entry(struct archive* writer, const fs::path& disk, const std::string& arcname,
                       Progress* progress) {
    std::error_code ec;
    fs::path target = fs::read_symlink(disk, ec);
    if (ec) throw fs::filesystem_error("readlink", disk, ec);
    std::string link = target.string();

    // The link target has to go in the entry's symlink field. Writing it as the
    // entry payload instead produced a link with an EMPTY target, so every
    // extracted dosdevices/c: (and ~/Music, ~/Documents, ...) came back broken.
    struct archive_entry* entry = make_entry(arcname, 0, AE_IFLNK, 0777u, mtime_of(disk));
    archive_entry_set_symlink(entry, link.c_str());
    write_header_or_throw(writer, entry, arcname);

    progress->done_bytes += static_cast<long long>(link.size());
    progress->report_backup(arcname);
}

void add_directory_entry(struct archive* writer, const fs::path& disk, const std::string& arcname,
                         Progress* progress) {
    std::error_code ec;
    fs::file_status st = fs::symlink_status(disk, ec);
    if (ec) throw fs::filesystem_error("stat", disk, ec);

    std::string name = arcname + "/";
    write_header_or_throw(writer, make_entry(name, 0, AE_IFDIR, perms_of(st), mtime_of(disk)), name);
    progress->report_backup(name);
}

// Python tarfile.add(): one filesystem object becomes one archive member,
// directories are walked recursively. Symlinks are stored as symlinks (the
// tarfile default), sockets/fifos/devices are skipped - reading those can
// block forever and a wine prefix never needs them in a backup.
void add_to_archive(struct archive* writer, const fs::path& disk, const std::string& arcname,
                    Progress* progress) {
    if (progress->task->cancel_requested()) throw TaskCancelledError();

    // The archive is written while the tree around it is being read. When the
    // user saves it INSIDE the prefix folder it is a regular file in this very
    // walk, and reading it back would copy the archive into itself - one pass
    // roughly doubles its size and the copy keeps growing, which is how a
    // 300 MB prefix turned into a multi-GB file. It is never an input.
    if (progress->dest && same_file(disk, *progress->dest)) return;

    std::error_code ec;
    fs::file_status st = fs::symlink_status(disk, ec);
    if (ec) throw fs::filesystem_error("stat", disk, ec);

    if (fs::is_directory(st)) {
        add_directory_entry(writer, disk, arcname, progress);

        fs::directory_iterator it(disk, fs::directory_options::skip_permission_denied, ec);
        if (ec) throw fs::filesystem_error("list directory", disk, ec);
        std::vector<fs::path> children;
        for (; it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) throw fs::filesystem_error("list directory", disk, ec);
            children.push_back(it->path());
        }
        if (ec) throw fs::filesystem_error("list directory", disk, ec);

        // Stable order, so two backups of the same prefix compare equal.
        std::sort(children.begin(), children.end());
        for (const fs::path& child : children) {
            add_to_archive(writer, child, arcname + "/" + child.filename().string(), progress);
        }
    } else if (fs::is_symlink(st)) {
        add_symlink_entry(writer, disk, arcname, progress);
    } else if (fs::is_regular_file(st)) {
        add_regular_file(writer, disk, arcname, progress);
    }
}

// manifest.json as an archive member of its own. Python writes it to a temp
// file first and tar.add()s that - keeping the text in memory here produces
// exactly the same archive without the temp file.
void add_text_entry(struct archive* writer, const std::string& arcname, const std::string& text) {
    write_header_or_throw(writer, make_entry(arcname, static_cast<la_int64_t>(text.size()), AE_IFREG,
                                             0644u, std::time(nullptr)),
                          arcname);
    if (!text.empty() && archive_write_data(writer, text.data(), text.size()) < 0) {
        throw std::runtime_error(archive_error(writer, "failed to write " + arcname));
    }
}

// Python run_backup_worker(): one .tar.gz with the prefix, the game folders
// that live outside of it, their icons and the manifest.
void run_backup_worker(const prefix::PrefixEntry& entry,
                       const std::vector<GameBackupInfo>& games_info, const fs::path& dest_path,
                       const tasklog::TaskPtr& task) {
    Progress progress;
    progress.task = task;
    progress.dest = dest_path;

    // The total has to be known before the archiving starts, otherwise the
    // progress bar could only ever show "how far along", never a percentage.
    // The walk is inside the try, so a cancel here is reported like any other
    // cancel instead of escaping as a raw error.
    std::vector<fs::path> size_sources;
    size_sources.push_back(fs::path(entry.prefix_path));
    for (const GameBackupInfo& g : games_info) {
        if (!g.inside_prefix) size_sources.push_back(g.folder_path);
        if (g.has_icon) size_sources.push_back(g.icon_path);
    }

    try {
        task->append_line("Calculating total size to back up...");
        progress.total_bytes = compute_total_size(size_sources, task, dest_path);
        task->append_line(progress.total_bytes > 0
                              ? "Total size: " +
                                    util::human_size(static_cast<double>(progress.total_bytes))
                              : "Total size: unknown");
        task->append_line("");

        json manifest = build_manifest(entry, games_info);

        std::error_code ec;
        fs::create_directories(dest_path.parent_path(), ec);

        WriteArchive archive(dest_path);
        struct archive* writer = archive.get();

        task->append_line("Archiving prefix files...");
        add_to_archive(writer, fs::path(entry.prefix_path), "prefix", &progress);

        for (const GameBackupInfo& g : games_info) {
            if (g.inside_prefix) {
                // Archiving it twice would only double the size of the backup.
                task->append_line("Skipping separate archive for '" + g.script_name +
                                  "' (already included inside the prefix folder).");
                continue;
            }
            task->append_line("Archiving game files: " + g.script_name + "...");
            add_to_archive(writer, g.folder_path, "games/" + g.script_name, &progress);
        }
        for (const GameBackupInfo& g : games_info) {
            if (!g.has_icon) continue;
            task->append_line("Archiving icon for '" + g.script_name + "'...");
            add_to_archive(writer, g.icon_path, "icons/" + g.script_name + ".png", &progress);
        }
        add_text_entry(writer, "manifest.json", manifest.dump(2));

        archive.finish();

        task->set_progress(100);
        task->append_line("");
        task->append_line("Done. " + std::to_string(progress.count) + " files, " +
                          util::human_size(static_cast<double>(progress.done_bytes)) + " total.");
        task->append_line("Backup saved to: " + dest_path.string());
        post_status("Backup of '" + entry.prefix_code + "' saved to " + dest_path.string(), "success");
    } catch (const TaskCancelledError&) {
        task->append_line("");
        task->append_line("Backup cancelled by user.");
        // Never leave a half-written archive behind (Python: dest_path.unlink()).
        std::error_code ec;
        if (fs::exists(dest_path, ec)) {
            fs::remove(dest_path, ec);
            if (!ec) task->append_line("Removed incomplete backup file: " + dest_path.string());
        }
        post_status("Backup of '" + entry.prefix_code + "' cancelled.", "warning");
    } catch (const std::exception& e) {
        std::string err = e.what();
        task->append_line("");
        task->append_line("ERROR: " + err);
        post_failure("Backup failed: " + err, "Backup Failed", "Backup failed:\n" + err);
    }
}

// ---- save/open choosers ----

// tkinter's asksaveasfilename: a save-style native chooser with a default
// file name and a *.tar.gz filter.
std::optional<std::string> choose_save_file(const std::string& title, const std::string& current_name,
                                            const std::string& filter_label,
                                            const std::vector<std::string>& patterns,
                                            GtkWindow* parent) {
    GtkFileChooserNative* native = gtk_file_chooser_native_new(
        title.c_str(), parent ? parent : ui::window, GTK_FILE_CHOOSER_ACTION_SAVE, "_Save", "_Cancel");
    GtkFileChooser* chooser = GTK_FILE_CHOOSER(native);

    gtk_file_chooser_set_current_name(chooser, current_name.c_str());
    gtk_file_chooser_set_do_overwrite_confirmation(chooser, TRUE);

    if (!patterns.empty()) {
        GtkFileFilter* filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, filter_label.c_str());
        for (const std::string& pattern : patterns) {
            gtk_file_filter_add_pattern(filter, pattern.c_str());
        }
        gtk_file_chooser_add_filter(chooser, filter);
    }
    GtkFileFilter* all_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(all_filter, "All Files");
    gtk_file_filter_add_pattern(all_filter, "*");
    gtk_file_chooser_add_filter(chooser, all_filter);

    std::optional<std::string> result;
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(native)) == GTK_RESPONSE_ACCEPT) {
        gchar* filename = gtk_file_chooser_get_filename(chooser);
        if (filename) {
            result = std::string(filename);
            g_free(filename);
        }
    }
    g_object_unref(native);
    return result;
}

// tkinter's defaultextension=".tar.gz": only appended when the typed name has
// no extension at all, so "mybackup.tgz" is kept as typed.
fs::path with_tar_gz_suffix(const fs::path& dest) {
    std::string name = dest.filename().string();
    if (name.find('.') == std::string::npos) return dest.parent_path() / (name + ".tar.gz");
    return dest;
}

void open_backup_prefix_dialog_impl(const prefix::PrefixEntry& entry, GtkWindow* parent) {
    if (warn_if_busy(parent)) return;

    fs::path prefix_path(entry.prefix_path);
    if (!fs::is_directory(prefix_path)) {
        dialogs::show_error("Error", "Prefix folder not found on disk:\n" + prefix_path.string(),
                            parent);
        return;
    }

    std::vector<GameBackupInfo> games_info = gather_prefix_backup_info(entry);

    // Confirmation first: the user sees which games are skipped / folded into
    // the prefix BEFORE the (potentially long) archiving starts.
    std::string message =
        "Prefix: " + entry.prefix_code + " (" + cfg::runner_display_name(entry.runner) + ")\n" +
        "Location: " + prefix_path.string() + "\n\n";
    if (!games_info.empty()) {
        message += "Game(s) that will be included:\n";
        for (const GameBackupInfo& g : games_info) {
            std::string tag;
            if (g.missing) {
                tag = "  (folder not found on disk - will be SKIPPED)";
            } else if (g.inside_prefix) {
                tag = "  (already inside the prefix - no extra space needed)";
            } else {
                tag = "  (backed up separately)";
            }
            message += "  - " + g.script_name + tag + "\n";
        }
    } else {
        message +=
            "No game is currently linked to this prefix - only the prefix itself will be backed up.\n";
    }
    message += "\nThis can take a while and produce a large file depending on the game size. Continue?";

    if (!dialogs::ask_yes_no("Confirm Backup", message, parent)) return;

    std::string default_name =
        "backup_" + entry.prefix_code + "_" + format_now("%Y%m%d_%H%M%S") + ".tar.gz";
    std::optional<std::string> chosen =
        choose_save_file("Save Backup As", default_name, "Backup Archive", {"*.tar.gz"}, parent);
    if (!chosen) return;
    fs::path dest_path = with_tar_gz_suffix(*chosen);

    // Missing folders are only warned about, they are never archived.
    games_info.erase(std::remove_if(games_info.begin(), games_info.end(),
                                    [](const GameBackupInfo& g) { return g.missing; }),
                     games_info.end());

    tasklog::TaskPtr task = tasklog::open(
        "Backup - " + entry.prefix_code, parent, true,
        "Hide this window. The backup keeps running - reopen it with \"Show Logs\" in the "
        "Prefix Configuration Manager.",
        "Stop the backup process currently running?\n\n"
        "The backup is stopped and the incomplete .tar.gz is deleted. The prefix and the game "
        "folders being backed up are not touched.");
    task->append_line("Starting backup of prefix '" + entry.prefix_code + "'...");
    task->append_line("Destination: " + dest_path.string());
    task->append_line("");

    tasklog::run("backup", entry.prefix_code, task,
                 [entry, games_info, dest_path](const tasklog::TaskPtr& t) {
                     run_backup_worker(entry, games_info, dest_path, t);
                 });
}

// ---- reading a backup archive ----

// One game as described by manifest.json.
struct ManifestGame {
    std::string script_name;
    std::string relative_exe;
    std::string exe_path;
    std::string launch_options;
    std::string comment;
    bool inside_prefix = false;
    std::string relative_to_prefix;
    bool has_icon = false;
};

std::vector<ManifestGame> manifest_games(const json& manifest) {
    std::vector<ManifestGame> games;
    const json& arr = json_arr(manifest, "games");
    if (!arr.is_array()) return games;
    for (const json& item : arr) {
        ManifestGame g;
        g.script_name = json_str(item, "script_name");
        if (g.script_name.empty()) continue;
        g.relative_exe = json_str(item, "relative_exe");
        g.exe_path = json_str(item, "exe_path");
        g.launch_options = json_str(item, "launch_options");
        g.comment = json_str(item, "comment");
        g.inside_prefix = json_bool(item, "inside_prefix");
        g.relative_to_prefix = json_str(item, "relative_to_prefix");
        g.has_icon = json_bool(item, "has_icon");
        games.push_back(std::move(g));
    }
    return games;
}

struct ArchiveContents {
    json manifest;
    long long total_size = 0;
};

// Python's read_archive(): manifest.json + the byte total of everything that
// will be extracted. tar.gz is a stream, so reaching manifest.json (written
// last) means decompressing the whole file first - which is why the caller shows
// a "please wait" dialog for it.
//
// `on_progress` (optional) is told how many members have been read and how
// many uncompressed bytes they add up to, so the caller's "please wait" dialog
// can show that something is happening. A .tar.gz has no knowable total up
// front (the compression ratio is only known once the whole file has been
// read), so this reports real numbers rather than a percentage that would lie.
using ScanProgress = std::function<void(long long members, long long uncompressed_bytes)>;

ArchiveContents read_backup_archive(const fs::path& archive_path,
                                    const ScanProgress& on_progress) {
    ArchiveContents out;
    ReadArchive archive(archive_path);
    struct archive* reader = archive.get();

    struct archive_entry* entry = nullptr;
    bool found = false;
    std::string text;
    char buffer[64 * 1024];
    long long scanned_members = 0;

    while (true) {
        int rc = archive_read_next_header(reader, &entry);
        if (rc == ARCHIVE_EOF) break;
        if (rc == ARCHIVE_WARN) continue;  // unusable header: skip that one member
        if (rc != ARCHIVE_OK) {
            throw std::runtime_error(archive_error(reader, "failed to read the backup archive"));
        }

        if (on_progress) {
            long long members = ++scanned_members;
            // -1 = the total over every filter, i.e. the bytes the decompressor
            // has produced so far (archive_position_uncompressed is deprecated).
            const la_int64_t uncompressed = archive_filter_bytes(reader, -1);
            on_progress(members, uncompressed > 0 ? static_cast<long long>(uncompressed) : 0);
        }

        const char* name_c = archive_entry_pathname(entry);
        std::string name = name_c ? name_c : "";
        if (starts_with(name, "./")) name = name.substr(2);

        if (name == "manifest.json") {
            text.clear();
            la_ssize_t got;
            while ((got = archive_read_data(reader, buffer, sizeof(buffer))) > 0) {
                text.append(buffer, static_cast<size_t>(got));
            }
            if (got < 0) {
                throw std::runtime_error(archive_error(reader, "failed to read manifest.json"));
            }
            out.manifest = json::parse(text, nullptr, false);
            if (out.manifest.is_discarded()) {
                throw std::runtime_error("manifest.json inside the archive is not valid JSON.");
            }
            found = true;
            continue;
        }

        if (archive_entry_filetype(entry) == AE_IFREG &&
            (starts_with(name, "prefix/") || starts_with(name, "games/"))) {
            out.total_size += archive_entry_size(entry);
        }
    }

    if (!found) throw std::runtime_error("manifest.json not found inside the archive.");
    return out;
}

// ---- restoring a backup archive ----

struct MemberRoute {
    fs::path dest;      // final file below the destination root
    fs::path root;      // destination root (checked again as a safety net)
    std::string display;  // path shown in the log
    std::string game;     // external game this member belongs to ("" = none)
    std::string icon;     // script whose icon this member is ("" = none)
};

// Where does one archive member go? Mirrors how Python's run_restore_worker()
// picks members by prefix: "prefix/" -> the new prefix folder,
// "games/<name>/" -> that game's own folder (only for games that really live
// outside the prefix), "icons/<name>.png" -> the icon folder. Anything else
// (manifest.json, unknown names) is not restored.
bool route_member(const std::string& raw_name, const fs::path& prefix_root, const fs::path& games_root,
                  const std::set<std::string>& external_games,
                  const std::set<std::string>& icon_scripts, MemberRoute* out) {
    std::string name = raw_name;
    while (starts_with(name, "./")) name = name.substr(2);
    while (!name.empty() && name.front() == '/') name.erase(name.begin());
    while (!name.empty() && name.back() == '/') name.pop_back();
    if (name.empty() || name == "." || name == "manifest.json") return false;

    auto build = [&](const fs::path& root, const std::string& rel, const std::string& display) {
        if (root.empty()) return false;
        fs::path safe = sanitize_entry_path(rel);
        if (safe.empty()) return false;  // the bare folder entry - it is created upfront
        fs::path dest = root / safe;
        if (!util::is_within(dest, root)) return false;  // never write outside the destination
        out->dest = dest;
        out->root = root;
        out->display = display;
        return true;
    };

    if (name == "prefix") return false;
    if (starts_with(name, "prefix/")) {
        std::string rel = name.substr(7);
        return build(prefix_root, rel, rel);
    }
    if (starts_with(name, "games/")) {
        std::string body = name.substr(6);
        size_t slash = body.find('/');
        std::string script = slash == std::string::npos ? body : body.substr(0, slash);
        std::string rel = slash == std::string::npos ? "" : body.substr(slash + 1);
        if (!external_games.count(script) || games_root.empty()) return false;
        if (!build(games_root / script, rel, rel)) return false;
        out->game = script;
        return true;
    }
    if (starts_with(name, "icons/")) {
        std::string body = name.substr(6);
        if (body.find('/') != std::string::npos) return false;
        if (!ends_with(body, ".png") || body.size() == 4) return false;
        std::string script = body.substr(0, body.size() - 4);
        if (!icon_scripts.count(script)) return false;
        if (!build(cfg::icon_dir, body, body)) return false;
        out->icon = script;
        return true;
    }
    return false;
}

void copy_entry_data(struct archive* reader, struct archive* disk, const tasklog::TaskPtr& task) {
    const void* buffer = nullptr;
    size_t size = 0;
    la_int64_t offset = 0;
    int rc;
    while ((rc = archive_read_data_block(reader, &buffer, &size, &offset)) == ARCHIVE_OK) {
        if (task->cancel_requested()) throw TaskCancelledError();
        if (size > 0 && archive_write_data(disk, buffer, size) < 0) {
            throw std::runtime_error(archive_error(disk, "failed to write entry data"));
        }
    }
    if (rc != ARCHIVE_EOF) {
        throw std::runtime_error(archive_error(reader, "failed to read entry data"));
    }
}

// Python run_restore_worker(): extract the prefix / game folders / icons to
// their new addresses, then register everything as new prefix + new games.
// Cancel is only checked while extracting: the launcher is NOT touched until
// the extraction finished completely, so a cancelled restore leaves files on
// disk but registers nothing.
void run_restore_worker(const fs::path& archive_path, const json& manifest,
                        const std::string& runner_key, const std::string& prefix_code,
                        const fs::path& target_prefix_path, const fs::path& games_dest_base,
                        long long total_size, const tasklog::TaskPtr& task) {
    std::vector<ManifestGame> games = manifest_games(manifest);

    Progress progress;
    progress.task = task;
    progress.total_bytes = total_size;

    std::set<std::string> external_games;
    std::set<std::string> icon_scripts;
    std::map<std::string, fs::path> game_dest_map;
    for (const ManifestGame& g : games) {
        if (g.inside_prefix) {
            // Its files come back with the prefix itself - only the script's
            // folder address has to be rebuilt.
            fs::path dir = g.relative_to_prefix.empty()
                               ? target_prefix_path
                               : target_prefix_path / g.relative_to_prefix;
            game_dest_map[g.script_name] = dir;
        } else {
            external_games.insert(g.script_name);
            if (!games_dest_base.empty()) {
                game_dest_map[g.script_name] = games_dest_base / g.script_name;
            }
        }
        if (g.has_icon) icon_scripts.insert(g.script_name);
    }

    try {
        std::error_code ec;
        fs::create_directories(target_prefix_path, ec);
        if (!games_dest_base.empty()) fs::create_directories(games_dest_base, ec);
        // Every destination exists before the first file lands in it.
        for (const auto& kv : game_dest_map) fs::create_directories(kv.second, ec);

        ReadArchive reader(archive_path);
        DiskWriter disk_writer;
        struct archive* disk = disk_writer.get();

        task->append_line("Extracting prefix files...");

        std::set<std::string> announced;
        struct archive_entry* entry = nullptr;
        while (true) {
            int rc = archive_read_next_header(reader.get(), &entry);
            if (rc == ARCHIVE_EOF) break;
            if (rc == ARCHIVE_WARN) continue;  // unusable header: skip that one member
            if (rc != ARCHIVE_OK) {
                throw std::runtime_error(
                    archive_error(reader.get(), "failed to read the backup archive"));
            }
            if (task->cancel_requested()) throw TaskCancelledError();

            const char* name_c = archive_entry_pathname(entry);
            MemberRoute route;
            if (!route_member(name_c ? name_c : "", target_prefix_path, games_dest_base, external_games,
                              icon_scripts, &route)) {
                continue;  // libarchive drops the payload when the next header is read
            }

            if (!route.game.empty() && announced.insert(route.game).second) {
                task->append_line("Extracting game files: " + route.game + "...");
            }
            if (!route.icon.empty()) {
                // Python copies the icon bytes by hand; icons are not part of
                // the progress percentage either.
                task->append_line("Extracting icon for '" + route.icon + "'...");
            } else {
                la_int64_t member_size = archive_entry_size(entry);
                if (member_size > 0) progress.done_bytes += member_size;
                progress.report_restore(route.display);
            }

            std::error_code sec;
            fs::create_directories(route.dest.parent_path(), sec);
            archive_entry_set_pathname(entry, route.dest.c_str());
            if (archive_write_header(disk, entry) != ARCHIVE_OK) {
                throw std::runtime_error(archive_error(disk, "failed to write " + route.display));
            }
            copy_entry_data(reader.get(), disk, task);
            archive_write_finish_entry(disk);
        }

        for (const ManifestGame& g : games) {
            if (!g.inside_prefix) continue;
            task->append_line("'" + g.script_name +
                              "' is already inside the restored prefix - no separate "
                              "extraction needed.");
        }

        // --- extraction is complete: only now the launcher registers it ---
        proton::record_prefix_usage(runner_key, prefix_code, target_prefix_path);

        json runner_cfg = cfg::load_runner_config();
        for (const ManifestGame& g : games) {
            auto it = game_dest_map.find(g.script_name);
            if (it == game_dest_map.end()) continue;
            const fs::path& game_dir = it->second;

            std::string exe_path;
            if (!g.relative_exe.empty()) {
                exe_path = (game_dir / g.relative_exe).string();
            } else {
                exe_path = g.exe_path;
                task->append_line("WARNING: could not determine the .exe location for '" +
                                  g.script_name +
                                  "' automatically - please check/fix it manually.");
            }

            scripts::RunnerChoice choice;
            choice.runner = runner_key;
            // The Proton build of the old machine is deliberately not carried
            // over: it only exists there, so it is asked for on first use.
            choice.proton_name = "";
            choice.proton_path = "";
            choice.prefix_code = prefix_code;
            choice.prefix_path = target_prefix_path.string();
            choice.launch_options = g.launch_options;
            choice.comment = g.comment;
            runner_cfg[g.script_name] = scripts::choice_to_json(choice);

            fs::path script_path = cfg::bashlaunch_dir / (g.script_name + ".sh");
            {
                std::ofstream out(script_path);
                if (!out) throw std::runtime_error("cannot write " + script_path.string());
                out << scripts::build_script_content(game_dir.string(), exe_path, choice);
            }
            // Python chmod(0o755) inside try/except: a failure here must not
            // undo a restore that already succeeded.
            std::error_code pec;
            fs::permissions(script_path,
                            fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                                fs::perms::others_read | fs::perms::others_exec,
                            pec);
        }
        cfg::save_runner_config(runner_cfg);

        task->set_progress(100);
        task->append_line("");
        task->append_line("Restore completed successfully.");
        if (runner_key != "wine") {
            task->append_line(
                "Note: this prefix's Proton build was not carried over from the old machine -");
            task->append_line(
                "you'll be asked to pick a locally installed Proton build the first time you");
            task->append_line("use Winecfg/Explorer/Winetricks/PLAY for it.");
        }
        post_status("Restore finished: '" + prefix_code + "' restored to " +
                        target_prefix_path.string(),
                    "success", /*refresh_list=*/true);
    } catch (const TaskCancelledError&) {
        task->append_line("");
        task->append_line(
            "Restore cancelled by user. Files already extracted so far remain on disk, but no "
            "game/prefix was registered in the launcher since the process did not finish.");
        post_status("Restore of '" + prefix_code + "' cancelled.", "warning");
    } catch (const std::exception& e) {
        std::string err = e.what();
        task->append_line("");
        task->append_line("ERROR: " + err);
        post_failure("Restore failed: " + err, "Restore Failed", "Restore failed:\n" + err);
    }
}

void start_restore_task(GtkWindow* parent, const fs::path& archive_path, const json& manifest,
                        const std::string& runner_key, const std::string& prefix_code,
                        const fs::path& target_prefix_path, const fs::path& games_dest_base,
                        long long total_size) {
    tasklog::TaskPtr task = tasklog::open(
        "Restore - " + prefix_code, parent, true,
        "Hide this window. The restore keeps running - reopen it with \"Show Logs\" in the "
        "Prefix Configuration Manager.",
        "Stop the restore process currently running?\n\n"
        "The restore is stopped. Files already extracted stay on disk, but no game/prefix is "
        "registered in the launcher because the restore did not finish.");
    task->append_line("Starting restore from: " + archive_path.string());
    task->append_line("Prefix destination: " + target_prefix_path.string());
    if (!games_dest_base.empty()) {
        task->append_line("Game(s) destination base folder: " + games_dest_base.string());
    }
    if (total_size > 0) {
        task->append_line("Total size to extract: " +
                          util::human_size(static_cast<double>(total_size)));
    }
    task->append_line("");

    tasklog::run("restore", prefix_code, task,
                 [archive_path, manifest, runner_key, prefix_code, target_prefix_path, games_dest_base,
                  total_size](const tasklog::TaskPtr& t) {
                     run_restore_worker(archive_path, manifest, runner_key, prefix_code,
                                        target_prefix_path, games_dest_base, total_size, t);
                 });
}

// ---- the restore options dialog ----

struct BrowseCtx {
    GtkWindow* dialog;
    GtkWidget* entry;
    const char* title;
};

void on_browse_clicked(GtkButton*, gpointer data) {
    auto* ctx = static_cast<BrowseCtx*>(data);
    std::string current = gtk_entry_get_text(GTK_ENTRY(ctx->entry));
    std::optional<std::string> chosen = dialogs::choose_folder(ctx->title, current, ctx->dialog);
    if (chosen) gtk_entry_set_text(GTK_ENTRY(ctx->entry), chosen->c_str());
}

bool dir_nonempty(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    return fs::directory_iterator(dir, ec) != fs::directory_iterator();
}

// Python show_restore_options_dialog(): once the manifest is read, ask for the
// new prefix code and the destination folders, check for conflicts, then start
// the actual restore.
void show_restore_options_dialog(GtkWindow* parent, const fs::path& archive_path,
                                 const ArchiveContents& contents) {
    const json& manifest = contents.manifest;
    std::string runner_key = json_str(manifest, "runner");
    if (runner_key != "wine" && runner_key != "protonge" && runner_key != "protoncachyos" &&
        runner_key != "steamproton") {
        dialogs::show_error("Error", "This backup file's format is not recognized/supported.", parent);
        return;
    }

    std::string orig_prefix_code = json_str(manifest, "prefix_code");
    if (orig_prefix_code.empty()) orig_prefix_code = "RestoredPrefix";

    std::vector<ManifestGame> games = manifest_games(manifest);
    std::vector<std::string> external_names;
    std::vector<std::string> inside_names;
    for (const ManifestGame& g : games) {
        if (g.inside_prefix) {
            inside_names.push_back(g.script_name);
        } else {
            external_names.push_back(g.script_name);
        }
    }

    // Reuse the original prefix code unless it is already taken on this
    // machine, otherwise suggest the next free one for this runner.
    std::set<std::string> known_codes;
    json runner_cfg = cfg::load_runner_config();
    if (runner_cfg.is_object()) {
        for (const auto& kv : runner_cfg.items()) {
            std::string code = kv.value().value("prefix_code", std::string());
            if (!code.empty()) known_codes.insert(code);
        }
    }
    json registry = cfg::load_prefix_registry();
    if (registry.is_object()) {
        for (const auto& kv : registry.items()) {
            std::string code = kv.value().value("prefix_code", std::string());
            if (!code.empty()) known_codes.insert(code);
        }
    }
    std::string suggested_code = known_codes.count(orig_prefix_code)
                                     ? proton::generate_next_prefix_code(runner_key)
                                     : orig_prefix_code;

    // Python: the runner's default root, overridden by a custom location when
    // one was configured.
    fs::path default_root = cfg::default_prefix_roots().at(runner_key);
    std::string custom_root = json_str(cfg::load_prefix_location_config(), runner_key);
    if (!custom_root.empty()) default_root = custom_root;
    fs::path default_games_root = fs::path(cfg::home_dir()) / "RestoredGames";

    GtkWidget* dialog = gtk_dialog_new_with_buttons("Restore Backup", parent ? parent : ui::window,
                                                    GTK_DIALOG_MODAL, "_Restore", GTK_RESPONSE_OK,
                                                    "_Cancel", GTK_RESPONSE_CANCEL, NULL);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), winmode::px(15));
    gtk_box_set_spacing(GTK_BOX(content), winmode::px(5));

    auto add = [content](GtkWidget* child) {
        gtk_box_pack_start(GTK_BOX(content), child, FALSE, FALSE, 0);
    };
    auto add_label = [&add](const std::string& text, bool wrap) {
        GtkWidget* label = gtk_label_new(text.c_str());
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        if (wrap) {
            gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
            gtk_label_set_max_width_chars(GTK_LABEL(label), 70);
        }
        add(label);
        return label;
    };

    add_label("Archive: " + archive_path.filename().string(), true);
    GtkWidget* runner_label = add_label("Runner: " + cfg::runner_display_name(runner_key), false);
    gtk_widget_set_margin_top(runner_label, winmode::px(4));

    if (!games.empty()) {
        std::vector<std::string> desc_lines;
        if (!inside_names.empty()) {
            desc_lines.push_back("Included inside the prefix (no extra folder needed): " +
                                 join(inside_names, ", "));
        }
        if (!external_names.empty()) {
            desc_lines.push_back("Stored separately, needs its own destination folder: " +
                                 join(external_names, ", "));
        }
        add_label(join(desc_lines, "\n"), true);
    } else {
        add_label("This backup contains only the prefix (no game linked).", true);
    }

    add_label("Prefix code (folder name) for the restored prefix:", false);
    GtkWidget* code_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(code_entry), 40);
    gtk_entry_set_text(GTK_ENTRY(code_entry), suggested_code.c_str());
    gtk_entry_set_activates_default(GTK_ENTRY(code_entry), TRUE);
    add(code_entry);

    if (suggested_code != orig_prefix_code) {
        add_label("Original code was '" + orig_prefix_code + "', already used on this machine - '" +
                      suggested_code + "' suggested instead. You can still change it.",
                  true);
    }

    add_label("Restore prefix into folder:", false);
    GtkWidget* prefix_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(prefix_entry), default_root.string().c_str());
    BrowseCtx prefix_ctx = {GTK_WINDOW(dialog), prefix_entry,
                            "Choose folder to restore the prefix into"};
    GtkWidget* prefix_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, winmode::px(6));
    gtk_box_pack_start(GTK_BOX(prefix_row), prefix_entry, TRUE, TRUE, 0);
    GtkWidget* prefix_browse = gtk_button_new_with_label("Browse...");
    gtk_widget_set_size_request(prefix_browse, winmode::px(90), -1);
    g_signal_connect(prefix_browse, "clicked", G_CALLBACK(on_browse_clicked), &prefix_ctx);
    gtk_box_pack_start(GTK_BOX(prefix_row), prefix_browse, FALSE, FALSE, 0);
    gtk_widget_set_margin_bottom(prefix_row, winmode::px(8));
    add(prefix_row);

    GtkWidget* games_entry = nullptr;
    BrowseCtx games_ctx = {};
    if (!external_names.empty()) {
        add_label("Restore game file(s) into folder (each game gets its own subfolder):", false);
        games_entry = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(games_entry), default_games_root.string().c_str());
        games_ctx = {GTK_WINDOW(dialog), games_entry, "Choose folder to restore game file(s) into"};
        GtkWidget* games_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, winmode::px(6));
        gtk_box_pack_start(GTK_BOX(games_row), games_entry, TRUE, TRUE, 0);
        GtkWidget* games_browse = gtk_button_new_with_label("Browse...");
        gtk_widget_set_size_request(games_browse, winmode::px(90), -1);
        g_signal_connect(games_browse, "clicked", G_CALLBACK(on_browse_clicked), &games_ctx);
        gtk_box_pack_start(GTK_BOX(games_row), games_browse, FALSE, FALSE, 0);
        gtk_widget_set_margin_bottom(games_row, winmode::px(8));
        add(games_row);
    }

    gtk_widget_show_all(dialog);

    std::string prefix_code;
    fs::path target_prefix_path;
    fs::path games_dest_base;
    bool started = false;

    // Every check runs inside the dialog loop (Python re-checks in do_restore)
    // so a refused confirmation or a typo keeps the dialog open.
    while (true) {
        if (gtk_dialog_run(GTK_DIALOG(dialog)) != GTK_RESPONSE_OK) break;
        if (warn_if_busy(GTK_WINDOW(dialog))) continue;

        prefix_code = trim(gtk_entry_get_text(GTK_ENTRY(code_entry)));
        if (prefix_code.empty()) {
            dialogs::show_info("Info", "Prefix code cannot be empty.", GTK_WINDOW(dialog));
            continue;
        }

        std::string prefix_dest = trim(gtk_entry_get_text(GTK_ENTRY(prefix_entry)));
        if (prefix_dest.empty()) {
            dialogs::show_info("Info", "Please choose a destination folder for the prefix.",
                               GTK_WINDOW(dialog));
            continue;
        }
        target_prefix_path = fs::path(prefix_dest) / prefix_code;

        std::vector<std::string> conflicting_scripts;
        for (const ManifestGame& g : games) {
            std::error_code ec;
            if (fs::exists(cfg::bashlaunch_dir / (g.script_name + ".sh"), ec)) {
                conflicting_scripts.push_back(g.script_name);
            }
        }
        if (!conflicting_scripts.empty()) {
            std::string msg =
                "The following game(s) already exist in this launcher (their launch script &\n"
                "runner settings will be REPLACED by this backup):\n\n";
            for (const std::string& s : conflicting_scripts) msg += "  - " + s + "\n";
            msg += "\nReplace them?";
            if (!dialogs::ask_yes_no("Replace Existing Game(s)?", msg, GTK_WINDOW(dialog))) continue;
        }

        if (dir_nonempty(target_prefix_path)) {
            std::string msg = "A prefix folder already exists at:\n" + target_prefix_path.string() +
                              "\n\nIts contents will be REPLACED/overwritten by the files from this "
                              "backup.\n\nDo you want to replace it?";
            if (!dialogs::ask_yes_no("Replace Existing Prefix?", msg, GTK_WINDOW(dialog))) continue;
        }

        games_dest_base.clear();
        if (!external_names.empty()) {
            std::string games_dest = trim(gtk_entry_get_text(GTK_ENTRY(games_entry)));
            if (games_dest.empty()) {
                dialogs::show_info("Info", "Please choose a destination folder for the game file(s).",
                                   GTK_WINDOW(dialog));
                continue;
            }
            games_dest_base = games_dest;

            std::vector<std::string> conflicting_folders;
            for (const std::string& s : external_names) {
                if (dir_nonempty(games_dest_base / s)) {
                    conflicting_folders.push_back((games_dest_base / s).string());
                }
            }
            if (!conflicting_folders.empty()) {
                std::string msg =
                    "The following game folder(s) already exist at the chosen destination and are\n"
                    "not empty. Their contents will be REPLACED/overwritten by this backup:\n\n";
                for (const std::string& s : conflicting_folders) msg += "  - " + s + "\n";
                msg += "\nDo you want to replace them?";
                if (!dialogs::ask_yes_no("Replace Existing Game Folder(s)?", msg, GTK_WINDOW(dialog))) {
                    continue;
                }
            }
        }

        started = true;
        break;
    }

    gtk_widget_destroy(dialog);
    if (!started) return;

    start_restore_task(parent, archive_path, manifest, runner_key, prefix_code, target_prefix_path,
                       games_dest_base, contents.total_size);
}

}  // namespace

void open_backup_prefix_dialog(const prefix::PrefixEntry& entry, GtkWindow* parent) {
    try {
        open_backup_prefix_dialog_impl(entry, parent);
    } catch (const std::exception& e) {
        std::string err = e.what();
        ui::set_status("Backup failed: " + err, "error");
        dialogs::show_error("Backup Failed", "Backup failed:\n" + err, parent);
    } catch (...) {
        ui::set_status("Backup failed with an unknown error.", "error");
        dialogs::show_error("Backup Failed", "Backup failed with an unknown error.", parent);
    }
}

void open_restore_backup_dialog(GtkWindow* parent) {
    try {
        if (warn_if_busy(parent)) return;

        // Python filedialog.askopenfilename(): a .tar.gz (or .tgz) backup.
        std::optional<std::string> chosen = dialogs::choose_open_file(
            "Select Backup Archive", "Backup Archive", {"*.tar.gz", "*.tgz"}, parent);
        if (!chosen) return;
        fs::path archive_path(*chosen);

        auto contents = std::make_shared<ArchiveContents>();
        auto error = std::make_shared<std::string>();
        // What the scan has read so far, shown under the wait dialog's bar.
        auto members = std::make_shared<std::atomic<long long>>(0);
        auto bytes = std::make_shared<std::atomic<long long>>(0);

        // Python run_with_loading_overlay(): opening a .tar.gz is a stream, so
        // finding manifest.json can mean reading the whole archive first. The
        // counter under the bar is what stops a large archive from looking like
        // a stuck launcher.
        dialogs::run_with_progress(
            "Opening Backup", "Reading " + archive_path.filename().string() + "...",
            [archive_path, contents, error, members, bytes]() {
                try {
                    *contents = read_backup_archive(
                        archive_path, [members, bytes](long long member_count, long long size) {
                            members->store(member_count);
                            bytes->store(size);
                        });
                } catch (const std::exception& e) {
                    *error = e.what();
                } catch (...) {
                    *error = "unknown error";
                }
            },
            [parent, archive_path, contents, error]() {
                if (!error->empty()) {
                    ui::set_status("Failed to read backup archive: " + *error, "error");
                    dialogs::show_error("Error", "Failed to read backup archive:\n" + *error, parent);
                    return;
                }
                show_restore_options_dialog(parent, archive_path, *contents);
            },
            parent, [members, bytes]() -> std::string {
                // Bytes first: they keep climbing while a big member is being
                // decompressed, where the member count can sit still.
                return util::human_size(static_cast<double>(bytes->load())) + " read (" +
                       std::to_string(members->load()) + " members)";
            });
    } catch (const std::exception& e) {
        std::string err = e.what();
        ui::set_status("Restore failed: " + err, "error");
        dialogs::show_error("Restore Failed", "Restore failed:\n" + err, parent);
    } catch (...) {
        ui::set_status("Restore failed with an unknown error.", "error");
        dialogs::show_error("Restore Failed", "Restore failed with an unknown error.", parent);
    }
}

}  // namespace backup
