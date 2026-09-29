#pragma once

// Stand-ins for tkinter's messagebox / filedialog / simpledialog.
// No colors or fonts are set here, everything is left to the system GTK theme.

#include <gtk/gtk.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace dialogs {

void show_info(const std::string& title, const std::string& message, GtkWindow* parent = nullptr);
void show_error(const std::string& title, const std::string& message, GtkWindow* parent = nullptr);
bool ask_yes_no(const std::string& title, const std::string& message, GtkWindow* parent = nullptr);

// Three-way question (Yes / No / Cancel) - Python's askyesnocancel().
// nullopt means the dialog was closed/escaped without an answer.
std::optional<bool> ask_yes_no_cancel(const std::string& title, const std::string& message,
                                      GtkWindow* parent = nullptr);

// Returns the entered text (trimmed) or nullopt when cancelled.
std::optional<std::string> ask_string(const std::string& title, const std::string& prompt,
                                      const std::string& initial_value = "",
                                      GtkWindow* parent = nullptr);

// initial_dir, when given, is the folder the chooser starts in (ignored when
// it does not exist).
std::optional<std::string> choose_open_file(const std::string& title, const std::string& filter_label,
                                            const std::vector<std::string>& patterns,
                                            GtkWindow* parent = nullptr,
                                            const std::string& initial_dir = "");

std::optional<std::string> choose_folder(const std::string& title, const std::string& initial_dir,
                                         GtkWindow* parent = nullptr);

// Modal "please wait" dialog with a pulsing progress bar while work() runs on a
// background thread; done() is then called back on the GTK main thread.
//
// `detail` is optional and is polled ON THE MAIN THREAD every ~150 ms; the
// string it returns is shown under the bar (e.g. "12,345 members - 247 MB
// read"). Use it when the job knows what it has done so far but not how much is
// left - a percentage would be a lie there. The bar keeps pulsing.
void run_with_progress(const std::string& title, const std::string& message,
                       std::function<void()> work, std::function<void()> done,
                       GtkWindow* parent = nullptr,
                       std::function<std::string()> detail = nullptr);

}  // namespace dialogs
