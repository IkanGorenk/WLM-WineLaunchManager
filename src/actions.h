#pragma once

// Handlers for the main window's buttons and menu items.
// Every entry point has the GTK callback signature so it can be connected
// directly with g_signal_connect().

#include <gtk/gtk.h>

namespace actions {

void run_script(GtkWidget* widget, gpointer data);
void view_logs(GtkWidget* widget, gpointer data);
void add_script(GtkWidget* widget, gpointer data);
void remove_script(GtkWidget* widget, gpointer data);
void rename_script(GtkWidget* widget, gpointer data);
void change_icon(GtkWidget* widget, gpointer data);
void open_file_manager(GtkWidget* widget, gpointer data);
void open_wine_prefix_folder(GtkWidget* widget, gpointer data);
void open_winecfg(GtkWidget* widget, gpointer data);
void run_exe_setup(GtkWidget* widget, gpointer data);

// Settings menu entries
void run_wine_uninstaller(GtkWidget* widget, gpointer data);
void run_wine_explorer(GtkWidget* widget, gpointer data);

// Settings -> "Download Proton": the two flavours stay separate entries, one per
// runner, so neither is hidden behind a picker.
void download_proton_ge(GtkWidget* widget, gpointer data);
void download_proton_cachyos(GtkWidget* widget, gpointer data);

// The window with one "Download from GitHub" button per runner. It is what the
// "Download from Online..." button of the Runner Options window opens.
void open_proton_download_window(GtkWidget* widget, gpointer data);

// G_CALLBACK for a "Close" button: closes the window the button is in.
//
// Use this instead of connecting gtk_widget_destroy to "clicked" - the signal
// passes the button, so that would destroy the button and leave the window open
// with a hole where the button used to be.
void close_window_from_button(GtkWidget* widget, gpointer data);

// SETTINGS -> "GOG Library...": the account's games, and installing one into a
// prefix of this launcher.
void open_gog_library(GtkWidget* widget, gpointer data);

// Settings -> "Runner Options": one window listing the installed Proton builds,
// with the actions that work on the selected runner (download, extract an
// archive into it, open its folder, remove it).
void open_runner_options_window(GtkWidget* widget, gpointer data);

// The same actions as menu items, each asking which runner it is for.
void extract_proton_archive(GtkWidget* widget, gpointer data);
void open_runner_folder(GtkWidget* widget, gpointer data);

// Deletes the chosen runner and all of its extracted builds. Prefixes that used
// it survive; they just have no build recorded, so the next use asks again.
void remove_runner(GtkWidget* widget, gpointer data);
void open_prefix_manager(GtkWidget* widget, gpointer data);
void refresh_list(GtkWidget* widget, gpointer data);

// "Config HUD" button next to the launch mode selector.
void config_hud(GtkWidget* widget, gpointer data);

void sort_by_selected(GtkComboBox* combo, gpointer data);

}  // namespace actions
