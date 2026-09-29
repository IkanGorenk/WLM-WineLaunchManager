#include "live_log.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

#include "config.h"
#include "dialogs.h"
#include "util.h"
#include "window_mode.h"

namespace live_log {

namespace {

// game name -> tracked process (only mutated from the GTK main thread)
std::map<std::string, GamePtr>& games() {
    static std::map<std::string, GamePtr> map;
    return map;
}

// Python: ANSI_ESCAPE_RE = r'\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])'
std::string strip_ansi(const std::string& input) {
    std::string out;
    size_t i = 0;
    while (i < input.size()) {
        if (input[i] == '\x1B') {
            if (i + 1 < input.size() && input[i + 1] == '[') {
                i += 2;
                while (i < input.size() && input[i] >= 0x30 && input[i] <= 0x3F) ++i;
                while (i < input.size() && input[i] >= 0x20 && input[i] <= 0x2F) ++i;
                if (i < input.size()) ++i;  // final byte
                continue;
            }
            if (i + 1 < input.size()) {
                unsigned char c = static_cast<unsigned char>(input[i + 1]);
                if ((c >= 0x40 && c <= 0x5A) || (c >= 0x5C && c <= 0x5F)) {
                    i += 2;
                    continue;
                }
            }
            ++i;
            continue;
        }
        out.push_back(input[i]);
        ++i;
    }
    return out;
}

// Python: chunk.decode("utf-8", errors="replace")
std::string utf8_replace_invalid(const std::string& input) {
    static const char* replacement = "\xEF\xBF\xBD";
    std::string out;
    size_t i = 0;
    while (i < input.size()) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        size_t len = 0;
        if (c < 0x80) {
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        }
        bool valid = len > 0 && i + len <= input.size();
        for (size_t k = 1; valid && k < len; ++k) {
            if ((static_cast<unsigned char>(input[i + k]) & 0xC0) != 0x80) valid = false;
        }
        if (!valid) {
            out += replacement;
            ++i;
            continue;
        }
        out.append(input, i, len);
        i += len;
    }
    return out;
}

std::string normalize_newlines(std::string text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            out.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::string read_text_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void stream_output(GamePtr entry, int master_fd, pid_t pid) {
    std::ofstream log_file(entry->log_path, std::ios::out | std::ios::trunc);
    bool exited = false;
    int status = 0;

    try {
        while (true) {
            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(master_fd, &read_set);
            struct timeval timeout = {0, 250000};  // 0.25s

            int ready = select(master_fd + 1, &read_set, nullptr, nullptr, &timeout);
            if (ready < 0) {
                if (errno == EINTR) continue;
                break;
            }
            bool have_data = ready > 0 && FD_ISSET(master_fd, &read_set);

            if (have_data) {
                char chunk[4096];
                ssize_t n = read(master_fd, chunk, sizeof(chunk));
                if (n < 0) {
                    if (errno == EIO) break;  // slave side closed
                    if (errno == EINTR) continue;
                    break;
                }
                if (n == 0) break;

                std::string text = normalize_newlines(
                    strip_ansi(utf8_replace_invalid(std::string(chunk, static_cast<size_t>(n)))));
                if (log_file) {
                    log_file << text;
                    log_file.flush();
                }
                std::lock_guard<std::mutex> lock(entry->mutex);
                entry->pending.push_back(text);
            }

            if (!exited) {
                pid_t waited = waitpid(pid, &status, WNOHANG);
                if (waited == pid) exited = true;
            }
            if (exited && !have_data) break;
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(entry->mutex);
        entry->pending.push_back(std::string("\n[launcher] Error reading process output: ") + e.what() +
                                 "\n");
    }

    close(master_fd);
    if (!exited) {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }

    int exit_code = 0;
    if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) exit_code = 128 + WTERMSIG(status);

    {
        std::lock_guard<std::mutex> lock(entry->mutex);
        entry->exit_code = exit_code;
        entry->pending.push_back("\n[launcher] Process exited (code " + std::to_string(exit_code) +
                                 ").\n");
        entry->finished = true;
    }
}

gboolean scroll_to_end(gpointer data) {
    GtkTextView* text_view = GTK_TEXT_VIEW(data);
    GtkTextBuffer* buffer = gtk_text_view_get_buffer(text_view);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_view_scroll_to_iter(text_view, &end, 0.0, FALSE, 0.0, 0.0);
    g_object_unref(text_view);
    return G_SOURCE_REMOVE;
}

void append_to_view(GamePtr entry, const std::string& text) {
    GtkTextView* text_view = GTK_TEXT_VIEW(entry->text_view);
    GtkTextBuffer* buffer = gtk_text_view_get_buffer(text_view);

    bool was_at_bottom = true;
    GtkAdjustment* vadj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(text_view));
    if (vadj) {
        was_at_bottom = (gtk_adjustment_get_value(vadj) + gtk_adjustment_get_page_size(vadj)) >=
                        (gtk_adjustment_get_upper(vadj) - 5);
    }

    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, text.c_str(), static_cast<gint>(text.size()));

    if (was_at_bottom) {
        g_object_ref(text_view);
        g_idle_add(scroll_to_end, text_view);
    }
}

struct OpenFileCtx {
    fs::path path;
    GtkWindow* parent = nullptr;
};

void on_clear_clicked(GtkButton*, gpointer data) {
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(data), "", -1);
}

void on_open_log_file_clicked(GtkButton*, gpointer data) {
    auto* ctx = static_cast<OpenFileCtx*>(data);
    if (fs::exists(ctx->path)) {
        int err = 0;
        util::open_path(ctx->path.string(), &err);
    } else {
        dialogs::show_info("Info", "No log file yet", ctx->parent);
    }
}

gboolean on_log_window_delete(GtkWidget* widget, GdkEvent*, gpointer) {
    gpointer data = g_object_get_data(G_OBJECT(widget), "log-entry");
    if (data) {
        GamePtr* entry = static_cast<GamePtr*>(data);
        if (*entry) {
            (*entry)->window = nullptr;
            (*entry)->text_view = nullptr;
            (*entry)->status_label = nullptr;
        }
    }
    return GDK_EVENT_PROPAGATE;  // let GTK destroy the window
}

void delete_holder(gpointer data) {
    delete static_cast<GamePtr*>(data);
}

void delete_open_ctx(gpointer data) {
    delete static_cast<OpenFileCtx*>(data);
}

}  // namespace

GamePtr launch_tracked(const std::string& game_name, const std::vector<std::string>& command,
                       int* exec_err, const util::EnvMap& extra_env) {
    if (exec_err) *exec_err = 0;
    if (command.empty()) {
        if (exec_err) *exec_err = EINVAL;
        return nullptr;
    }

    int master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd < 0) {
        if (exec_err) *exec_err = errno;
        return nullptr;
    }
    if (grantpt(master_fd) != 0 || unlockpt(master_fd) != 0) {
        int e = errno;
        close(master_fd);
        if (exec_err) *exec_err = e;
        return nullptr;
    }
    char* slave_name = ptsname(master_fd);
    if (!slave_name) {
        int e = errno;
        close(master_fd);
        if (exec_err) *exec_err = e;
        return nullptr;
    }
    int slave_fd = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave_fd < 0) {
        int e = errno;
        close(master_fd);
        if (exec_err) *exec_err = e;
        return nullptr;
    }

    int err_pipe[2];
    if (pipe(err_pipe) != 0) {
        int e = errno;
        close(slave_fd);
        close(master_fd);
        if (exec_err) *exec_err = e;
        return nullptr;
    }
    fcntl(err_pipe[1], F_SETFD, FD_CLOEXEC);

    util::EnvMap child_env = util::clean_env();
    for (const auto& kv : extra_env) child_env[kv.first] = kv.second;
    util::EnvBlock env_block(child_env);

    pid_t pid = fork();
    if (pid < 0) {
        int e = errno;
        close(err_pipe[0]);
        close(err_pipe[1]);
        close(slave_fd);
        close(master_fd);
        if (exec_err) *exec_err = e;
        return nullptr;
    }

    if (pid == 0) {
        close(err_pipe[0]);
        setsid();
        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);
        if (slave_fd > STDERR_FILENO) close(slave_fd);
        close(master_fd);

        std::vector<char*> argv;
        argv.reserve(command.size() + 1);
        for (const auto& arg : command) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);

        execvpe(argv[0], argv.data(), env_block.data());
        int e = errno;
        ssize_t ignored = write(err_pipe[1], &e, sizeof(e));
        (void)ignored;
        _exit(127);
    }

    close(slave_fd);
    close(err_pipe[1]);
    int child_errno = 0;
    ssize_t got = read(err_pipe[0], &child_errno, sizeof(child_errno));
    close(err_pipe[0]);

    if (got > 0) {
        close(master_fd);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        if (exec_err) *exec_err = child_errno;
        return nullptr;
    }

    GamePtr entry = std::make_shared<RunningGame>();
    entry->pid = pid;
    entry->log_path = cfg::logs_dir / (game_name + ".log");
    games()[game_name] = entry;

    std::thread(stream_output, entry, master_fd, pid).detach();
    return entry;
}

gboolean poll_queues(gpointer) {
    const int max_lines = cfg::MAX_LOG_BUFFER_LINES;

    for (auto& kv : games()) {
        const GamePtr& entry = kv.second;
        if (!entry) continue;

        std::string new_text;
        bool finished = false;
        {
            std::lock_guard<std::mutex> lock(entry->mutex);
            finished = entry->finished;
            if (!entry->pending.empty()) {
                for (const auto& line : entry->pending) new_text += line;
                entry->buffer.insert(entry->buffer.end(), entry->pending.begin(),
                                     entry->pending.end());
                entry->pending.clear();
                if (static_cast<int>(entry->buffer.size()) > max_lines) {
                    entry->buffer.erase(entry->buffer.begin(),
                                        entry->buffer.end() - max_lines);
                }
            }
        }

        if (!new_text.empty() && entry->text_view) {
            append_to_view(entry, new_text);
        }

        if (finished && entry->status_label) {
            gtk_label_set_text(GTK_LABEL(entry->status_label), "\u26AA Finished");
            gtk_style_context_add_class(gtk_widget_get_style_context(entry->status_label),
                                        "dim-label");
        }
    }
    return G_SOURCE_CONTINUE;
}

void open_log_window(const std::string& game_name) {
    GamePtr entry;
    auto it = games().find(game_name);
    if (it != games().end()) entry = it->second;

    fs::path log_path = cfg::logs_dir / (game_name + ".log");

    if (entry && entry->window) {
        gtk_window_present(GTK_WINDOW(entry->window));
        return;
    }

    GtkWidget* win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    std::string title = "Logs - " + game_name;
    gtk_window_set_title(GTK_WINDOW(win), title.c_str());
    gtk_window_set_default_size(GTK_WINDOW(win), 800, 500);
    gtk_widget_set_size_request(win, winmode::px(400), winmode::px(250));

    GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(win), outer);

    GtkWidget* top_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, winmode::px(6));
    gtk_container_set_border_width(GTK_CONTAINER(top_bar), winmode::px(8));
    gtk_box_pack_start(GTK_BOX(outer), top_bar, FALSE, FALSE, 0);

    GtkWidget* name_label = gtk_label_new(nullptr);
    gchar* escaped_name = g_markup_escape_text(game_name.c_str(), -1);
    std::string markup = std::string("<b>") + escaped_name + "</b>";
    g_free(escaped_name);
    gtk_label_set_markup(GTK_LABEL(name_label), markup.c_str());
    gtk_label_set_xalign(GTK_LABEL(name_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(top_bar), name_label, TRUE, TRUE, 0);

    bool is_live = false;
    if (entry) {
        std::lock_guard<std::mutex> lock(entry->mutex);
        is_live = !entry->finished;
    }
    std::string state_text = is_live ? "\U0001F7E2 Running (live)"
                                     : "\u26AA Not running (last saved log)";
    GtkWidget* run_state_label = gtk_label_new(state_text.c_str());
    if (!is_live) {
        gtk_style_context_add_class(gtk_widget_get_style_context(run_state_label), "dim-label");
    }
    gtk_box_pack_start(GTK_BOX(top_bar), run_state_label, FALSE, FALSE, 0);

    GtkWidget* scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(outer), scrolled, TRUE, TRUE, 0);

    GtkWidget* text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(text_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_NONE);
    gtk_container_set_border_width(GTK_CONTAINER(text_view), winmode::px(4));
    gtk_container_add(GTK_CONTAINER(scrolled), text_view);

    GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));

    auto set_initial = [&](const std::string& content) {
        gtk_text_buffer_set_text(buffer, content.c_str(), static_cast<gint>(content.size()));
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(buffer, &end);
        gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(text_view), &end, 0.0, FALSE, 0.0, 0.0);
    };

    if (entry) {
        std::string initial;
        {
            std::lock_guard<std::mutex> lock(entry->mutex);
            for (const auto& line : entry->buffer) initial += line;
        }
        if (initial.empty()) initial = "(waiting for output...)\n";
        set_initial(initial);
        entry->window = win;
        entry->text_view = text_view;
        entry->status_label = run_state_label;
    } else if (fs::exists(log_path)) {
        std::string content = read_text_file(log_path);
        if (content.empty()) {
            content = "[launcher] Could not read log file\n";
        }
        set_initial(content);
    } else {
        set_initial("(No logs yet - launch this game at least once first)\n");
    }

    GtkWidget* bottom_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, winmode::px(8));
    gtk_container_set_border_width(GTK_CONTAINER(bottom_bar), winmode::px(8));
    gtk_box_pack_start(GTK_BOX(outer), bottom_bar, FALSE, FALSE, 0);

    GtkWidget* clear_btn = gtk_button_new_with_label("Clear View");
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(on_clear_clicked), buffer);
    gtk_box_pack_start(GTK_BOX(bottom_bar), clear_btn, FALSE, FALSE, 0);

    auto* file_ctx = new OpenFileCtx{log_path, GTK_WINDOW(win)};
    g_object_set_data_full(G_OBJECT(win), "log-open-file-ctx", file_ctx, delete_open_ctx);

    GtkWidget* open_file_btn = gtk_button_new_with_label("Open Log File");
    g_signal_connect(open_file_btn, "clicked", G_CALLBACK(on_open_log_file_clicked), file_ctx);
    gtk_box_pack_start(GTK_BOX(bottom_bar), open_file_btn, FALSE, FALSE, 0);

    if (entry) {
        g_object_set_data_full(G_OBJECT(win), "log-entry", new GamePtr(entry), delete_holder);
    }
    g_signal_connect(win, "delete-event", G_CALLBACK(on_log_window_delete), nullptr);

    gtk_widget_show_all(win);
}

}  // namespace live_log
