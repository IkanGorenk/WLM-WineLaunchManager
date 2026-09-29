#pragma once

// Main window widgets + the UI-level helpers that operate on them.

#include <gtk/gtk.h>

#include <optional>
#include <string>

#include "config.h"

namespace ui {

// ---- widgets (created by build_window) ----
extern GtkWindow* window;
extern GtkWidget* status_label;
extern GtkWidget* game_title_label;
extern GtkWidget* icon_image;
extern GtkWidget* info_label;
extern GtkListStore* list_store;
extern GtkTreeView* tree;
extern GtkComboBoxText* launch_mode_combo;
extern GtkComboBoxText* sort_combo;
extern GtkPaned* main_paned;

// Builds the whole main window (header, toolbar, paned content, buttons).
// Builds the main window at the standard windowed size (see
// winmode::standard_size). There is no saved geometry any more.
void build_window();

void set_status(const std::string& text, const std::string& kind = "info");
void set_game_title(const std::string& text);
void reset_game_details();
void update_script_list(const std::string& sort_order = "ascending");
void load_icon(const std::string& script_name);

std::optional<std::string> get_selected_script_name();
std::string launch_mode_text();
void select_script(const std::string& name);

}  // namespace ui
