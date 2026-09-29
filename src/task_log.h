#pragma once

// Log window for Python-free background tasks (backup/restore/downloads),
// plus the "only one prefix backup/restore at a time" registry.
// C++ port of open_task_log_window()/start_prefix_task_thread()/
// prefix_task_is_busy()/_mark_prefix_task_finished() in launcher.py.

#include <gtk/gtk.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace tasklog {

// One task log window. append_line()/set_progress() are safe to call from any
// thread; everything else must be used from the GTK main thread. The widget
// pointers below are only touched from the main thread and are cleared again
// when the window is destroyed.
class Task {
public:
    // Queue one log line. The line is picked up by the window's 150 ms poll,
    // exactly like the Python queue Tk drains; lines produced after the window
    // was closed are dropped instead of piling up in memory.
    void append_line(const std::string& line);

    // Progress bar value (0..100). Switches the bar from its "busy" pulse to a
    // determinate bar the first time it is used. Safe from any thread.
    void set_progress(double percent);

    // Current transfer rate in bytes per second, shown next to the percentage.
    // Pass 0 to hide the figure again (a task that is not a download). Safe
    // from any thread.
    void set_speed(double bytes_per_second);

    // Back to the indeterminate state: the bar pulses again and the percentage
    // and speed are cleared. Used when a task moves from a measurable phase (the
    // download) to one with no total to count against (extracting). Safe from
    // any thread.
    void set_busy();

    // Set by the window's Cancel button; workers poll it and stop as soon as
    // they see it.
    bool cancel_requested() const { return cancel_requested_->load(); }
    void request_cancel() { cancel_requested_->store(true); }

    // Called once on the main thread when the worker is done: disables Cancel
    // and lets Close / the window manager really close the window again.
    void mark_finished();

    // Raise the (possibly hidden) window - the manager's "Show Logs".
    void show();

    bool finished() const { return finished_; }

    // The window still exists and can be raised (or closed for real). false once
    // it was really destroyed - Close only hides a window whose task runs.
    bool window_alive() const { return window != nullptr; }

    // Flush queued lines + progress into the widgets (main thread, called by
    // the poll timer).
    void drain();

    // Close the window for real, even while the task is still running.
    void destroy();

    // What the Cancel confirmation says. Set once by open() from the caller that
    // knows what the task does - a download, a backup and a restore each clean
    // up differently, so one shared wording is always wrong for two of them.
    void set_cancel_message(std::string text) { cancel_message_ = std::move(text); }
    const std::string& cancel_message() const { return cancel_message_; }

    // ---- wiring done by open()/the destroy handler ----
    GtkWidget* window = nullptr;
    GtkWidget* cancel_button = nullptr;
    GtkWidget* text_view = nullptr;
    GtkTextBuffer* buffer = nullptr;
    GtkWidget* progress_bar = nullptr;
    GtkWidget* pct_label = nullptr;
    GtkWidget* speed_label = nullptr;

private:
    guint timer_id_ = 0;
    bool pulse_ = true;  // still indeterminate (no progress reported yet)
    bool finished_ = false;
    std::string cancel_message_ = "Stop the process currently running?";

    std::shared_ptr<std::atomic<bool>> cancel_requested_ =
        std::make_shared<std::atomic<bool>>(false);

    std::mutex mutex_;
    std::vector<std::string> pending_;
    double pending_progress_ = 0.0;
    bool has_pending_progress_ = false;
    double pending_speed_ = 0.0;
    bool has_pending_speed_ = false;
    bool pending_busy_ = false;

public:
    void set_timer(guint id) { timer_id_ = id; }
    guint timer_id() const { return timer_id_; }
    void clear_window();  // called from the window's destroy handler
};

using TaskPtr = std::shared_ptr<Task>;

// Opens a non-modal task log window (progress bar + monospace log + Close, and
// Cancel when cancellable). While the task is still running, closing the window
// (Close button or the WM's X) only hides it, so the progress survives and the
// background task keeps going - whoever opened the window brings it back with
// its own button ("Show Download Log" / the manager's "Show Logs"). Once the
// task is finished, closing really closes it. modal_parent is the window this
// one belongs to.
//
// close_hint is the tooltip on Close. cancel_message is what the confirmation
// before stopping the task says - it has to describe THAT task ("the download is
// aborted and the temporary archive is deleted" is wrong for a backup, where a
// half-written .tar.gz is removed instead). Empty means a neutral wording.
TaskPtr open(const std::string& title, GtkWindow* modal_parent, bool cancellable,
             const std::string& close_hint = std::string(),
             const std::string& cancel_message = std::string());

// Runs work() on a background thread bound to task; when it returns, the task
// is marked finished on the main thread and the busy registry is released.
// Exceptions never escape the thread.
void run(const std::string& kind, const std::string& label, const TaskPtr& task,
         std::function<void(const TaskPtr&)> work);

// True while a backup/restore is still running in this session - only one is
// allowed at a time. kind/label describe what is running.
bool is_busy();
std::string busy_label();
std::string busy_kind();

// Raise the active task's log window (manager's "Show Logs" button).
void show_active();

}  // namespace tasklog
