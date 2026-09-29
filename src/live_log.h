#pragma once

// Real-time log streaming: every launched game gets a background thread that
// reads its pty output, writes it to ~/wlm/logs/<game>.log and feeds any open
// log viewer window.

#include <gtk/gtk.h>

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <sys/types.h>

#include "util.h"

namespace live_log {

namespace fs = std::filesystem;

struct RunningGame {
    pid_t pid = -1;
    fs::path log_path;

    std::mutex mutex;
    std::vector<std::string> pending;  // lines waiting to be shown
    std::vector<std::string> buffer;   // last MAX_LOG_BUFFER_LINES lines

    GtkWidget* window = nullptr;
    GtkWidget* text_view = nullptr;
    GtkWidget* status_label = nullptr;

    bool finished = false;
    int exit_code = 0;
};

using GamePtr = std::shared_ptr<RunningGame>;

// Runs `command` attached to a new pseudo terminal and starts streaming its
// output. Returns nullptr and reports the exec errno through *exec_err on failure.
// extra_env is merged over the clean environment (used by Winetricks, which has
// to be pointed at the WINE binary / WINEPREFIX of one specific prefix).
GamePtr launch_tracked(const std::string& game_name, const std::vector<std::string>& command,
                       int* exec_err, const util::EnvMap& extra_env = {});

// Periodic (GLib timeout) drain of every game's pending output.
gboolean poll_queues(gpointer data);

// Opens (or focuses) the log viewer window for a game.
void open_log_window(const std::string& game_name);

}  // namespace live_log
