#include "dialogs.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <thread>

#include "ui.h"
#include "window_mode.h"

namespace dialogs {

namespace {

GtkWindow* effective_parent(GtkWindow* parent) {
    return parent ? parent : ui::window;
}

std::string trim(const std::string& s) {
    auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

}  // namespace

void show_info(const std::string& title, const std::string& message, GtkWindow* parent) {
    GtkWidget* dialog = gtk_message_dialog_new(effective_parent(parent), GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s", title.c_str());
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", message.c_str());
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

void show_error(const std::string& title, const std::string& message, GtkWindow* parent) {
    GtkWidget* dialog = gtk_message_dialog_new(effective_parent(parent), GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", title.c_str());
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", message.c_str());
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

bool ask_yes_no(const std::string& title, const std::string& message, GtkWindow* parent) {
    GtkWidget* dialog = gtk_message_dialog_new(effective_parent(parent), GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO, "%s", title.c_str());
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", message.c_str());
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return response == GTK_RESPONSE_YES;
}

std::optional<bool> ask_yes_no_cancel(const std::string& title, const std::string& message,
                                      GtkWindow* parent) {
    GtkWidget* dialog = gtk_dialog_new_with_buttons(title.c_str(), effective_parent(parent),
                                                    static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                                                                GTK_DIALOG_DESTROY_WITH_PARENT),
                                                    "_Yes", GTK_RESPONSE_YES,
                                                    "_No", GTK_RESPONSE_NO,
                                                    "_Cancel", GTK_RESPONSE_CANCEL, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_YES);

    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), winmode::px(10));
    gtk_box_set_spacing(GTK_BOX(content), winmode::px(6));

    GtkWidget* label = gtk_label_new(message.c_str());
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 60);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 0);
    gtk_widget_show_all(dialog);

    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if (response == GTK_RESPONSE_YES) return true;
    if (response == GTK_RESPONSE_NO) return false;
    return std::nullopt;  // closed / Escape
}

std::optional<std::string> ask_string(const std::string& title, const std::string& prompt,
                                      const std::string& initial_value, GtkWindow* parent) {
    GtkWidget* dialog = gtk_dialog_new_with_buttons(title.c_str(), effective_parent(parent),
                                                    static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                                                                GTK_DIALOG_DESTROY_WITH_PARENT),
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_OK", GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);

    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), winmode::px(10));
    gtk_box_set_spacing(GTK_BOX(content), winmode::px(6));

    GtkWidget* label = gtk_label_new(prompt.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 0);

    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), initial_value.c_str());
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 40);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 0);

    gtk_widget_show_all(dialog);
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));

    std::optional<std::string> result;
    if (response == GTK_RESPONSE_OK) {
        result = trim(gtk_entry_get_text(GTK_ENTRY(entry)));
    }
    gtk_widget_destroy(dialog);
    return result;
}

std::optional<std::string> choose_open_file(const std::string& title, const std::string& filter_label,
                                            const std::vector<std::string>& patterns,
                                            GtkWindow* parent, const std::string& initial_dir) {
    GtkWidget* dialog = gtk_file_chooser_dialog_new(title.c_str(), effective_parent(parent),
                                                    GTK_FILE_CHOOSER_ACTION_OPEN,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_Open", GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

    if (!patterns.empty()) {
        GtkFileFilter* filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, filter_label.c_str());
        for (const auto& pattern : patterns) {
            gtk_file_filter_add_pattern(filter, pattern.c_str());
        }
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
    }

    GtkFileFilter* all_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(all_filter, "All Files");
    gtk_file_filter_add_pattern(all_filter, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), all_filter);

    // Start where the caller expects the user to be looking - without this the
    // chooser opens on "Other Locations", which is a lot of clicking for a
    // folder that is known.
    if (!initial_dir.empty()) {
        std::error_code ec;
        if (std::filesystem::is_directory(initial_dir, ec)) {
            gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), initial_dir.c_str());
        }
    }

    std::optional<std::string> result;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        gchar* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) {
            result = std::string(filename);
            g_free(filename);
        }
    }
    gtk_widget_destroy(dialog);
    return result;
}

std::optional<std::string> choose_folder(const std::string& title, const std::string& initial_dir,
                                         GtkWindow* parent) {
    GtkWidget* dialog = gtk_file_chooser_dialog_new(title.c_str(), effective_parent(parent),
                                                    GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_Select", GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

    if (!initial_dir.empty()) {
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), initial_dir.c_str());
    }

    std::optional<std::string> result;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        gchar* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) {
            result = std::string(filename);
            g_free(filename);
        }
    }
    gtk_widget_destroy(dialog);
    return result;
}

namespace {

struct ProgressState {
    GtkWidget* dialog = nullptr;
    GtkWidget* bar = nullptr;
    GtkWidget* detail_label = nullptr;
    guint pulse_id = 0;
    guint poll_id = 0;
    std::function<void()> work;
    std::function<void()> done;
    // Asked on the MAIN thread for a short "what has happened so far" line
    // shown under the bar. It has to be cheap and thread-safe (it reads a
    // counter the worker keeps updating).
    std::function<std::string()> detail;
};

gboolean pulse_cb(gpointer data) {
    auto* state = static_cast<ProgressState*>(data);
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(state->bar));
    return G_SOURCE_CONTINUE;
}

// Refreshes the "so far" line under the bar. The bar itself keeps pulsing: a
// .tar.gz cannot be measured honestly against its own size (the compression
// ratio is unknown until the end), so this shows activity rather than a
// percentage that would lie.
gboolean poll_detail_cb(gpointer data) {
    auto* state = static_cast<ProgressState*>(data);
    if (state->detail_label == nullptr || !state->detail) return G_SOURCE_CONTINUE;
    const std::string text = state->detail();
    if (!text.empty()) gtk_label_set_text(GTK_LABEL(state->detail_label), text.c_str());
    return G_SOURCE_CONTINUE;
}

gboolean finish_cb(gpointer data) {
    auto* state = static_cast<ProgressState*>(data);
    if (state->pulse_id != 0) {
        g_source_remove(state->pulse_id);
        state->pulse_id = 0;
    }
    if (state->poll_id != 0) {
        g_source_remove(state->poll_id);
        state->poll_id = 0;
    }
    gtk_widget_destroy(state->dialog);
    std::function<void()> done = std::move(state->done);
    delete state;
    if (done) done();
    return G_SOURCE_REMOVE;
}

void worker(ProgressState* state) {
    try {
        state->work();
    } catch (const std::exception& e) {
        std::cerr << "[launcher] background task failed: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "[launcher] background task failed with an unknown error\n";
    }
    g_idle_add(finish_cb, state);
}

}  // namespace

void run_with_progress(const std::string& title, const std::string& message,
                       std::function<void()> work, std::function<void()> done,
                       GtkWindow* parent, std::function<std::string()> detail) {
    auto* state = new ProgressState();
    state->work = std::move(work);
    state->done = std::move(done);
    if (detail) state->detail = std::move(detail);

    state->dialog = gtk_dialog_new_with_buttons(title.c_str(), effective_parent(parent),
                                                static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                                                            GTK_DIALOG_DESTROY_WITH_PARENT),
                                                NULL, NULL);
    gtk_window_set_deletable(GTK_WINDOW(state->dialog), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(state->dialog), FALSE);

    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(state->dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), winmode::px(20));
    gtk_box_set_spacing(GTK_BOX(content), winmode::px(10));

    GtkWidget* label = gtk_label_new(message.c_str());
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 0);

    GtkWidget* hint = gtk_label_new("This can take a while for large files.\n"
                                    "Please wait, the launcher is not frozen.");
    gtk_label_set_justify(GTK_LABEL(hint), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(content), hint, FALSE, FALSE, 0);

    state->bar = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(content), state->bar, FALSE, FALSE, 0);

    if (state->detail) {
        state->detail_label = gtk_label_new("");
        gtk_label_set_justify(GTK_LABEL(state->detail_label), GTK_JUSTIFY_CENTER);
        gtk_style_context_add_class(gtk_widget_get_style_context(state->detail_label),
                                    "dim-label");
        gtk_box_pack_start(GTK_BOX(content), state->detail_label, FALSE, FALSE, 0);
    }

    gtk_widget_show_all(state->dialog);
    state->pulse_id = g_timeout_add(100, pulse_cb, state);
    if (state->detail) state->poll_id = g_timeout_add(150, poll_detail_cb, state);

    std::thread(worker, state).detach();
}

}  // namespace dialogs
