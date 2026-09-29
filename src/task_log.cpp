#include "task_log.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <cstdio>
#include <thread>
#include <utility>

#include "dialogs.h"
#include "ui.h"
#include "util.h"
#include "window_mode.h"

namespace tasklog {

namespace {

// Keeps a Task alive for as long as a GTK signal/timer that points at it does;
// freed by the GDestroyNotify when the source or the window goes away.
struct Holder {
    TaskPtr task;
};

Holder* make_holder(const TaskPtr& task) { return new Holder{task}; }
void free_holder(gpointer data) { delete static_cast<Holder*>(data); }
// GClosureNotify variant for g_signal_connect_data().
void free_holder_closure(gpointer data, GClosure*) { delete static_cast<Holder*>(data); }

// The single backup/restore tracked for this session (main thread only, so no
// locking - run()'s worker only hands the finished task back through an idle).
TaskPtr active_task;
std::string active_kind;
std::string active_label;
bool active_finished = true;

// delete-event (the WM's X button): a task that is still running only gets its
// window hidden, so the progress survives and whoever owns it can raise the
// window again. A finished window is really closed.
gboolean on_delete_event(GtkWidget* widget, GdkEvent*, gpointer data) {
    Holder* holder = static_cast<Holder*>(data);
    if (!holder->task->finished()) {
        gtk_widget_hide(widget);
        return TRUE;  // block the close
    }
    return FALSE;
}

// The Close button: while the task runs, closing only hides the window (the
// download's owner brings it back with its "Show Download Log" button, the
// backup/restore with the manager's "Show Logs"); a finished window closes for
// real.
void on_close_clicked(GtkButton*, gpointer data) {
    Holder* holder = static_cast<Holder*>(data);
    TaskPtr task = holder->task;
    if (!task->finished()) {
        if (task->window) gtk_widget_hide(task->window);
        return;
    }
    task->destroy();
}

void on_cancel_clicked(GtkButton*, gpointer data) {
    Holder* holder = static_cast<Holder*>(data);
    TaskPtr task = holder->task;
    if (task->finished()) return;
    if (!dialogs::ask_yes_no("Cancel", task->cancel_message(), GTK_WINDOW(task->window))) {
        return;
    }
    task->request_cancel();
    task->append_line("");
    task->append_line("Cancelling... please wait for the current step to stop.");
    if (task->cancel_button) {
        gtk_widget_set_sensitive(task->cancel_button, FALSE);
        gtk_button_set_label(GTK_BUTTON(task->cancel_button), "Cancelling...");
    }
}

// The window is really gone: stop the poll timer and drop the widget references
// so late append_line() calls from the worker are simply ignored.
//
// Note that "closed while the task runs" never gets here - on_delete_event and
// on_close_clicked only hide the window in that case, so this is the end of the
// line for both the widgets and the timer.
void on_destroy(GtkWidget*, gpointer data) {
    Holder* holder = static_cast<Holder*>(data);
    holder->task->clear_window();
}

// 150 ms poll - the C++ equivalent of Tk's root.after() loop in the Python
// launcher: drains queued lines, applies the newest progress value and keeps
// the indeterminate bar moving until a real percentage shows up.
gboolean poll(gpointer data) {
    Holder* holder = static_cast<Holder*>(data);
    holder->task->drain();
    return G_SOURCE_CONTINUE;
}

// g_idle_add_full handler marking a finished worker (main thread).
gboolean mark_finished_idle(gpointer data) {
    Holder* holder = static_cast<Holder*>(data);
    TaskPtr task = holder->task;  // copied out; free_holder runs right after
    if (active_task == task) active_finished = true;
    task->mark_finished();
    return G_SOURCE_REMOVE;
}

}  // namespace

void Task::append_line(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (window == nullptr) return;  // window already closed: drop the line
    pending_.push_back(line);
}

void Task::set_progress(double percent) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (window == nullptr) return;
    pending_progress_ = std::max(0.0, std::min(100.0, percent));
    has_pending_progress_ = true;
}

void Task::set_speed(double bytes_per_second) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (window == nullptr) return;
    pending_speed_ = bytes_per_second > 0.0 ? bytes_per_second : 0.0;
    has_pending_speed_ = true;
}

void Task::set_busy() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (window == nullptr) return;
    pending_busy_ = true;
}

void Task::mark_finished() {
    finished_ = true;
    if (cancel_button) gtk_widget_set_sensitive(cancel_button, FALSE);
}

void Task::show() {
    if (window == nullptr) return;
    gtk_widget_show(window);
    gtk_window_present(GTK_WINDOW(window));
}

void Task::destroy() {
    if (window == nullptr) return;
    gtk_widget_destroy(window);
}

void Task::clear_window() {
    if (timer_id_ != 0) {
        g_source_remove(timer_id_);  // frees the timer's Holder too
        timer_id_ = 0;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    window = nullptr;
    cancel_button = nullptr;
    text_view = nullptr;
    buffer = nullptr;
    progress_bar = nullptr;
    pct_label = nullptr;
    speed_label = nullptr;
}

void Task::drain() {
    std::vector<std::string> lines;
    double progress = 0.0;
    bool has_progress = false;
    double speed = 0.0;
    bool has_speed = false;
    bool back_to_busy = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (window == nullptr) return;
        lines.swap(pending_);
        has_progress = has_pending_progress_;
        progress = pending_progress_;
        has_pending_progress_ = false;
        has_speed = has_pending_speed_;
        speed = pending_speed_;
        has_pending_speed_ = false;
        back_to_busy = pending_busy_;
        pending_busy_ = false;
    }

    if (buffer != nullptr && !lines.empty()) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(buffer, &end);
        for (const std::string& line : lines) {
            std::string with_newline = line + "\n";
            gtk_text_buffer_insert(buffer, &end, with_newline.c_str(),
                                   static_cast<gint>(with_newline.size()));
        }
        if (text_view != nullptr) {
            gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(text_view), &end, 0.0, FALSE, 0.0, 0.0);
        }
    }

    // A phase with nothing to count against (extracting): drop the percentage
    // and the rate, and let the bar pulse again until a real value arrives.
    if (back_to_busy) {
        pulse_ = true;
        has_progress = false;
        has_speed = false;
        if (progress_bar != nullptr) {
            gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(progress_bar), FALSE);
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
        }
        if (pct_label != nullptr) gtk_label_set_text(GTK_LABEL(pct_label), "");
        if (speed_label != nullptr) gtk_label_set_text(GTK_LABEL(speed_label), "");
    }

    if (has_progress && progress_bar != nullptr) {
        if (pulse_) {
            // First real percentage: stop the busy pulse for good.
            pulse_ = false;
            gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(progress_bar), TRUE);
        }
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), progress / 100.0);
        if (pct_label != nullptr) {
            char text[16];
            std::snprintf(text, sizeof(text), "%.0f%%", progress);
            gtk_label_set_text(GTK_LABEL(pct_label), text);
        }
    } else if (pulse_ && progress_bar != nullptr) {
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(progress_bar));
    }

    // Live transfer rate, e.g. "3.4 MB/s". Only a download sets it, so the
    // label stays empty for a backup or a restore.
    if (has_speed && speed_label != nullptr) {
        if (speed > 0.0) {
            const std::string text = util::human_size(speed) + "/s";
            gtk_label_set_text(GTK_LABEL(speed_label), text.c_str());
            gtk_widget_set_tooltip_text(speed_label, "Current download speed");
        } else {
            gtk_label_set_text(GTK_LABEL(speed_label), "");
        }
    }
}

TaskPtr open(const std::string& title, GtkWindow* modal_parent, bool cancellable,
             const std::string& close_hint, const std::string& cancel_message) {
    auto task = std::make_shared<Task>();
    if (!cancel_message.empty()) task->set_cancel_message(cancel_message);

    GtkWidget* win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), title.c_str());
    gtk_window_set_default_size(GTK_WINDOW(win), 800, 480);
    gtk_widget_set_size_request(win, winmode::px(480), winmode::px(300));
    if (modal_parent) {
        gtk_window_set_transient_for(GTK_WINDOW(win), modal_parent);
        gtk_window_set_destroy_with_parent(GTK_WINDOW(win), FALSE);
    }

    GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(outer), winmode::px(10));
    gtk_container_add(GTK_CONTAINER(win), outer);

    // ---- progress bar + percentage ----
    GtkWidget* progress_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, winmode::px(6));
    gtk_widget_set_margin_bottom(progress_row, winmode::px(8));
    gtk_box_pack_start(GTK_BOX(outer), progress_row, FALSE, FALSE, 0);

    GtkWidget* progress_bar = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(progress_row), progress_bar, TRUE, TRUE, 0);

    GtkWidget* pct_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(pct_label), 1.0f);
    gtk_label_set_width_chars(GTK_LABEL(pct_label), 6);
    gtk_box_pack_start(GTK_BOX(progress_row), pct_label, FALSE, FALSE, 0);

    // Live transfer rate ("3.4 MB/s"), filled in by set_speed() - a download
    // only. Dim-label so it reads as a figure next to the bar, not a headline.
    GtkWidget* speed_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(speed_label), 1.0f);
    gtk_label_set_width_chars(GTK_LABEL(speed_label), 11);
    gtk_style_context_add_class(gtk_widget_get_style_context(speed_label), "dim-label");
    gtk_box_pack_start(GTK_BOX(progress_row), speed_label, FALSE, FALSE, 0);

    // ---- the log itself ----
    GtkWidget* scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(outer), scrolled, TRUE, TRUE, 0);

    GtkWidget* text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(text_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_NONE);
    gtk_container_add(GTK_CONTAINER(scrolled), text_view);
    GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));

    // ---- Cancel / Close ----
    GtkWidget* close_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, winmode::px(6));
    gtk_widget_set_margin_top(close_row, winmode::px(8));
    gtk_widget_set_halign(close_row, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(outer), close_row, FALSE, FALSE, 0);

    GtkWidget* cancel_btn = nullptr;
    if (cancellable) {
        cancel_btn = gtk_button_new_with_label("Cancel");
        gtk_widget_set_size_request(cancel_btn, winmode::px(110), -1);
        g_signal_connect_data(cancel_btn, "clicked", G_CALLBACK(on_cancel_clicked),
                              make_holder(task), free_holder_closure, G_CONNECT_DEFAULT);
        gtk_box_pack_start(GTK_BOX(close_row), cancel_btn, FALSE, FALSE, 0);
    }

    GtkWidget* close_btn = gtk_button_new_with_label("Close");
    gtk_widget_set_size_request(close_btn, winmode::px(110), -1);
    // While the task runs this only hides the window, which used to look like a
    // Close button that did nothing at all - so say what it really does.
    if (!close_hint.empty()) gtk_widget_set_tooltip_text(close_btn, close_hint.c_str());
    g_signal_connect_data(close_btn, "clicked", G_CALLBACK(on_close_clicked), make_holder(task),
                          free_holder_closure, G_CONNECT_DEFAULT);
    gtk_box_pack_start(GTK_BOX(close_row), close_btn, FALSE, FALSE, 0);

    task->window = win;
    task->cancel_button = cancel_btn;
    task->text_view = text_view;
    task->buffer = buffer;
    task->progress_bar = progress_bar;
    task->pct_label = pct_label;
    task->speed_label = speed_label;
    task->set_timer(g_timeout_add_full(G_PRIORITY_DEFAULT, 150, poll, make_holder(task),
                                       free_holder));

    g_signal_connect_data(win, "delete-event", G_CALLBACK(on_delete_event), make_holder(task),
                          free_holder_closure, G_CONNECT_DEFAULT);
    g_signal_connect_data(win, "destroy", G_CALLBACK(on_destroy), make_holder(task),
                          free_holder_closure, G_CONNECT_DEFAULT);

    // Opened next to the window that asked for it (Python: +60/+60 of root).
    GtkWidget* anchor = GTK_WIDGET(modal_parent ? modal_parent : ui::window);
    gint root_x = 0;
    gint root_y = 0;
    if (anchor) gtk_window_get_position(GTK_WINDOW(anchor), &root_x, &root_y);
    gtk_window_move(GTK_WINDOW(win), root_x + winmode::px(60), root_y + winmode::px(60));
    gtk_widget_show_all(win);

    return task;
}

void run(const std::string& kind, const std::string& label, const TaskPtr& task,
         std::function<void(const TaskPtr&)> work) {
    active_task = task;
    active_kind = kind;
    active_label = label;
    active_finished = false;

    std::thread([task, work]() {
        try {
            work(task);
        } catch (const std::exception& e) {
            task->append_line(std::string("[launcher] error: ") + e.what());
        } catch (...) {
            task->append_line("[launcher] error: unknown error");
        }
        // Back on the main thread: release the busy slot and re-enable Close.
        Holder* holder = make_holder(task);
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, mark_finished_idle, holder, free_holder);
    }).detach();
}

bool is_busy() { return active_task && !active_finished; }

std::string busy_label() { return active_label; }

std::string busy_kind() { return active_kind; }

void show_active() {
    if (active_task && active_task->window_alive()) {
        active_task->show();
    } else {
        dialogs::show_info("Show Logs", "No backup or restore process is currently running.",
                           ui::window);
    }
}

}  // namespace tasklog
