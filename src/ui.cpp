#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sys/stat.h>
#include <vector>

#include <gdk-pixbuf/gdk-pixbuf.h>

#include "actions.h"
#include "config.h"
#include "live_log.h"
#include "util.h"
#include "window_mode.h"

namespace ui {

// ---- widget globals (created by build_window) ----
GtkWindow* window = nullptr;
GtkWidget* status_label = nullptr;
GtkWidget* game_title_label = nullptr;
GtkWidget* icon_image = nullptr;
GtkWidget* info_label = nullptr;
GtkListStore* list_store = nullptr;
GtkTreeView* tree = nullptr;
GtkComboBoxText* launch_mode_combo = nullptr;
GtkComboBoxText* sort_combo = nullptr;
GtkPaned* main_paned = nullptr;

namespace {

namespace fs = std::filesystem;

// ---- widgets whose size/padding tracks the auto-scale (see window_mode) ----
GtkWidget* header_box = nullptr;
GtkWidget* toolbar_box = nullptr;
GtkWidget* left_panel = nullptr;
GtkWidget* controls_box = nullptr;
GtkWidget* launch_label = nullptr;
GtkWidget* right_panel = nullptr;
GtkWidget* button_panel = nullptr;
GtkWidget* btn_row1 = nullptr;
GtkWidget* btn_row2 = nullptr;
GtkWidget* icon_frame = nullptr;
GtkTreeViewColumn* no_column = nullptr;
GtkTreeViewColumn* name_column = nullptr;

void add_menu_item(GtkWidget* menu, const char* label, void (*handler)(GtkWidget*, gpointer)) {
    GtkWidget* item = gtk_menu_item_new_with_label(label);
    g_signal_connect(item, "activate", G_CALLBACK(handler), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

// Clicking an already-selected game a second time deselects it, instead of
// doing nothing (GtkTreeView's default with a single selection).
gboolean on_tree_button_press(GtkWidget*, GdkEventButton* event, gpointer) {
    if (event->button != 1) return GDK_EVENT_PROPAGATE;

    GtkTreePath* path = nullptr;
    gtk_tree_view_get_path_at_pos(tree, static_cast<gint>(event->x), static_cast<gint>(event->y),
                                  &path, nullptr, nullptr, nullptr);
    if (!path) return GDK_EVENT_PROPAGATE;

    GtkTreeSelection* selection = gtk_tree_view_get_selection(tree);
    GtkTreeModel* model = nullptr;
    GtkTreeIter iter;
    bool selected_row = gtk_tree_selection_get_selected(selection, &model, &iter);

    bool clicked_selected = false;
    if (selected_row) {
        GtkTreePath* selected_path = gtk_tree_model_get_path(model, &iter);
        clicked_selected = gtk_tree_path_compare(selected_path, path) == 0;
        gtk_tree_path_free(selected_path);
    }
    gtk_tree_path_free(path);

    if (clicked_selected) {
        gtk_tree_selection_unselect_all(selection);
        return GDK_EVENT_STOP;  // swallow the click so it doesn't re-select the row
    }
    return GDK_EVENT_PROPAGATE;
}

void on_selection_changed(GtkTreeSelection*, gpointer);

gboolean set_initial_paned_position(gpointer) {
    gint total_width = 0;
    gtk_window_get_size(window, &total_width, nullptr);
    gtk_paned_set_position(main_paned, static_cast<gint>(total_width * 0.72));
    return G_SOURCE_REMOVE;
}

std::string format_size(off_t size_bytes) {
    double size_kb = static_cast<double>(size_bytes) / 1024.0;
    double size_mb = size_kb / 1024.0;
    char buffer[64];
    if (size_mb > 1.0) {
        std::snprintf(buffer, sizeof(buffer), "%.2f MB", size_mb);
    } else if (size_kb > 1.0) {
        std::snprintf(buffer, sizeof(buffer), "%.2f KB", size_kb);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%lld bytes", static_cast<long long>(size_bytes));
    }
    return std::string(buffer);
}

struct SelectCtx {
    const std::string* name = nullptr;
    GtkTreeView* tree = nullptr;
};

gboolean select_row_cb(GtkTreeModel* model, GtkTreePath*, GtkTreeIter* iter, gpointer data) {
    auto* ctx = static_cast<SelectCtx*>(data);
    gchar* value = nullptr;
    gtk_tree_model_get(model, iter, 1, &value, -1);
    bool match = value && *ctx->name == value;
    g_free(value);
    if (match) {
        gtk_tree_selection_select_iter(gtk_tree_view_get_selection(ctx->tree), iter);
        return TRUE;
    }
    return FALSE;
}

}  // namespace

void set_status(const std::string& text, const std::string& kind) {
    // Uses the GTK theme's own semantic style classes instead of hardcoded
    // colors, so feedback always matches the active system theme.
    GtkStyleContext* ctx = gtk_widget_get_style_context(status_label);
    for (const char* css_class : {"error", "warning", "dim-label"}) {
        gtk_style_context_remove_class(ctx, css_class);
    }
    if (kind == "error") {
        gtk_style_context_add_class(ctx, "error");
    } else if (kind == "warning") {
        gtk_style_context_add_class(ctx, "warning");
    } else if (kind == "secondary") {
        gtk_style_context_add_class(ctx, "dim-label");
    }
    gtk_label_set_text(GTK_LABEL(status_label), text.c_str());
}

void set_game_title(const std::string& text) {
    gchar* escaped = g_markup_escape_text(text.c_str(), -1);
    std::string markup = std::string("<b>") + escaped + "</b>";
    g_free(escaped);
    gtk_label_set_markup(GTK_LABEL(game_title_label), markup.c_str());
}

void reset_game_details() {
    set_game_title("No Game Selected");
    gtk_image_clear(GTK_IMAGE(icon_image));
    gtk_label_set_text(GTK_LABEL(info_label), "Select a game to view details");
}

void update_script_list(const std::string& sort_order) {
    gtk_list_store_clear(list_store);

    std::vector<std::string> names;
    std::error_code ec;
    if (fs::is_directory(cfg::bashlaunch_dir, ec)) {
        for (const auto& entry : fs::directory_iterator(cfg::bashlaunch_dir, ec)) {
            if (entry.is_regular_file() && entry.path().extension() == ".sh") {
                names.push_back(entry.path().stem().string());
            }
        }
    }
    std::sort(names.begin(), names.end(),
              [](const std::string& a, const std::string& b) { return util::lower(a) < util::lower(b); });
    if (sort_order == "descending") {
        std::reverse(names.begin(), names.end());
    }

    int index = 1;
    for (const auto& name : names) {
        GtkTreeIter iter;
        gtk_list_store_append(list_store, &iter);
        gtk_list_store_set(list_store, &iter, 0, index, 1, name.c_str(), -1);
        ++index;
    }

    if (names.empty()) reset_game_details();
}

void load_icon(const std::string& script_name) {
    fs::path icon_path = cfg::icon_dir / (script_name + ".png");
    const int icon_size = winmode::px(cfg::ICON_WIDTH);  // grows with the window

    GError* error = nullptr;
    GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file(icon_path.string().c_str(), &error);
    if (error) {
        g_error_free(error);
        pixbuf = nullptr;
    }

    if (!pixbuf) {
        gtk_image_clear(GTK_IMAGE(icon_image));  // missing or unloadable icon
        return;
    }

    if (gdk_pixbuf_get_width(pixbuf) != icon_size || gdk_pixbuf_get_height(pixbuf) != icon_size) {
        GdkPixbuf* scaled =
            gdk_pixbuf_scale_simple(pixbuf, icon_size, icon_size, GDK_INTERP_BILINEAR);
        g_object_unref(pixbuf);
        pixbuf = scaled;
    }

    gtk_image_set_from_pixbuf(GTK_IMAGE(icon_image), pixbuf);
    g_object_unref(pixbuf);
}

std::optional<std::string> get_selected_script_name() {
    if (!tree) return std::nullopt;
    GtkTreeSelection* selection = gtk_tree_view_get_selection(tree);
    GtkTreeModel* model = nullptr;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(selection, &model, &iter)) return std::nullopt;

    gchar* name = nullptr;
    gtk_tree_model_get(model, &iter, 1, &name, -1);
    std::string result = name ? name : "";
    g_free(name);
    return result;
}

std::string launch_mode_text() {
    if (!launch_mode_combo) return "";
    gchar* text = gtk_combo_box_text_get_active_text(launch_mode_combo);
    std::string result = text ? text : "";
    g_free(text);
    return result;
}

void select_script(const std::string& name) {
    SelectCtx ctx{&name, tree};
    gtk_tree_model_foreach(GTK_TREE_MODEL(list_store), select_row_cb, &ctx);
}

namespace {

void on_selection_changed(GtkTreeSelection*, gpointer) {
    std::optional<std::string> name = get_selected_script_name();
    if (!name) {
        reset_game_details();
        return;
    }

    set_game_title(*name);
    load_icon(*name);  // show the icon as soon as possible

    fs::path script_path = cfg::bashlaunch_dir / (*name + ".sh");
    if (!fs::exists(script_path)) {
        gtk_label_set_text(GTK_LABEL(info_label),
                           "File information not available (script file missing)");
        return;
    }

    try {
        struct stat st;
        if (::stat(script_path.c_str(), &st) != 0) {
            gtk_label_set_text(GTK_LABEL(info_label), "File information unavailable");
            return;
        }

        std::time_t mtime = st.st_mtime;
        std::tm local_time{};
        localtime_r(&mtime, &local_time);
        char mod_time[32];
        std::strftime(mod_time, sizeof(mod_time), "%Y-%m-%d %H:%M:%S", &local_time);

        // Second line of the script holds the working folder (cd "...").
        std::string folder_path;
        std::ifstream in(script_path);
        std::string line;
        int line_no = 0;
        while (std::getline(in, line)) {
            ++line_no;
            if (line_no == 2) {
                folder_path = line;
                std::string trimmed = folder_path;
                size_t begin = trimmed.find_first_not_of(" \t\r");
                size_t end = trimmed.find_last_not_of(" \t\r");
                trimmed = (begin == std::string::npos) ? "" : trimmed.substr(begin, end - begin + 1);
                size_t pos = trimmed.find("cd \"");
                if (pos != std::string::npos) trimmed.erase(pos, 4);
                trimmed.erase(std::remove(trimmed.begin(), trimmed.end(), '"'), trimmed.end());
                folder_path = trimmed;
                break;
            }
        }

        std::string text = "Script File: " + *name + ".sh\n" + "Location: " + folder_path + "\n" +
                           "Last Modified: " + mod_time + "\n" + "Size: " + format_size(st.st_size);
        gtk_label_set_text(GTK_LABEL(info_label), text.c_str());
    } catch (const std::exception& e) {
        std::string text = std::string("File information unavailable: ") + e.what();
        gtk_label_set_text(GTK_LABEL(info_label), text.c_str());
    }
}

// Re-applies everything in the layout that is authored at the design size
// (1000x720) for the current window_mode scale. Font sizes are handled globally
// by window_mode's CSS provider; this covers the pixel values GTK does not
// derive from the font: the icon, paddings, spacings and column widths.
void apply_scale(double scale) {
    if (!header_box) return;  // window not built yet

    auto s = [scale](int design_px) {
        return static_cast<int>(std::lround(design_px * scale));
    };

    gtk_container_set_border_width(GTK_CONTAINER(header_box), s(8));

    gtk_widget_set_margin_start(toolbar_box, s(15));
    gtk_widget_set_margin_end(toolbar_box, s(15));
    gtk_widget_set_margin_top(toolbar_box, s(2));

    gtk_widget_set_margin_start(GTK_WIDGET(main_paned), s(15));
    gtk_widget_set_margin_end(GTK_WIDGET(main_paned), s(15));
    gtk_widget_set_margin_bottom(GTK_WIDGET(main_paned), s(15));

    gtk_widget_set_margin_end(left_panel, s(10));
    gtk_box_set_spacing(GTK_BOX(left_panel), s(8));
    gtk_box_set_spacing(GTK_BOX(controls_box), s(6));
    gtk_widget_set_margin_start(launch_label, s(15));

    gtk_container_set_border_width(GTK_CONTAINER(right_panel), s(15));
    gtk_box_set_spacing(GTK_BOX(right_panel), s(8));
    gtk_box_set_spacing(GTK_BOX(button_panel), s(6));
    gtk_box_set_spacing(GTK_BOX(btn_row1), s(4));
    gtk_box_set_spacing(GTK_BOX(btn_row2), s(4));

    gtk_widget_set_size_request(icon_frame, s(cfg::ICON_WIDTH), s(cfg::ICON_HEIGHT));

    gtk_tree_view_column_set_fixed_width(no_column, s(40));
    gtk_tree_view_column_set_min_width(no_column, s(40));
    gtk_tree_view_column_set_min_width(name_column, s(200));

    // The icon is a scaled pixbuf, so it has to be rebuilt at the new size.
    if (std::optional<std::string> selected = get_selected_script_name()) {
        load_icon(*selected);
    }
}

}  // namespace

// The application icon, looked for in the places it can actually be depending
// on how the launcher was started or installed:
//
//   1. $WLM_ICON                    - an explicit override
//   2. next to the executable       - "make install" puts it there
//   3. the project directory        - running "./wlm-launcher" from a checkout
//   4. /usr/share/icons/hicolor/... - a system install
//
// Without this the window and the taskbar entry show a generic GTK placeholder.
// A missing file is not an error: the launcher simply runs without an icon.
void apply_window_icon(GtkWindow* window) {
    std::vector<fs::path> candidates;

    if (const char* override_path = g_getenv("WLM_ICON")) {
        if (*override_path != '\0') candidates.emplace_back(override_path);
    }

    // Directory of the running executable, so the icon is found no matter what
    // the current working directory happens to be.
    if (const char* exe = g_file_read_link("/proc/self/exe", nullptr)) {
        const fs::path exe_dir = fs::path(exe).parent_path();
        g_free(const_cast<char*>(exe));
        candidates.push_back(exe_dir / "winelaunchmanager.png");
        candidates.push_back(exe_dir / "share" / "winelaunchmanager" / "winelaunchmanager.png");
    }

    // Running straight from a checkout (including cmake's build/ directory).
    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    if (!ec) {
        candidates.push_back(cwd / "winelaunchmanager.png");
        candidates.push_back(cwd.parent_path() / "winelaunchmanager.png");
    }

    // "make install" puts the artwork under the data directory rather than next
    // to the binary, so both the direct copy and the hicolor theme entry are
    // looked for there.
    fs::path data_home;
    if (const char* xdg = g_get_user_data_dir()) data_home = xdg;
    if (data_home.empty()) {
        const char* home = g_get_home_dir();
        if (home != nullptr) data_home = fs::path(home) / ".local" / "share";
    }
    if (!data_home.empty()) {
        candidates.push_back(data_home / "winelaunchmanager" / "winelaunchmanager.png");
        for (const char* size : {"256x256", "128x128", "48x48"}) {
            candidates.push_back(data_home / "icons" / "hicolor" / size / "apps" /
                                 "wine-launcher.png");
        }
    }

    candidates.emplace_back("/usr/share/icons/hicolor/250x250/apps/wine-launcher.png");
    candidates.emplace_back("/usr/share/icons/hicolor/256x256/apps/wine-launcher.png");
    candidates.emplace_back("/usr/share/icons/hicolor/scalable/apps/wine-launcher.png");
    candidates.emplace_back("/usr/share/pixmaps/wine-launcher.png");

    for (const fs::path& candidate : candidates) {
        if (!fs::is_regular_file(candidate, ec)) continue;

        GError* error = nullptr;
        GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file(candidate.c_str(), &error);
        if (pixbuf == nullptr) {
            if (error != nullptr) g_error_free(error);
            continue;
        }
        gtk_window_set_icon(window, pixbuf);
        g_object_unref(pixbuf);
        return;
    }
}

void build_window() {
    window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    gtk_window_set_title(window, "Wine Launch Manager");
    // The standard windowed size, shrunk to fit a display that is smaller than
    // the design size. The same numbers are used as the window's MINIMUM, which
    // is what makes the titlebar maximize button work: a window manager cannot
    // grant less than the minimum, so a minimum taller than the display made it
    // drop the maximize request and the button looked dead.
    int width = winmode::DESIGN_W;
    int height = winmode::DESIGN_H;
    winmode::standard_size(&width, &height);
    gtk_window_set_default_size(window, width, height);
    gtk_widget_set_size_request(GTK_WIDGET(window), width, height);
    gtk_window_set_position(window, GTK_WIN_POS_CENTER);

    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);

    apply_window_icon(window);

    // ---- root box ----
    GtkWidget* root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), root_box);

    // ---- header ----
    header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(header_box), 8);
    gtk_box_pack_start(GTK_BOX(root_box), header_box, FALSE, FALSE, 0);

    GtkWidget* title_label = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(title_label),
                         "<span size='large' weight='bold'>WINE LAUNCH MANAGER</span>");
    gtk_box_pack_start(GTK_BOX(header_box), title_label, FALSE, FALSE, 0);

    status_label = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(status_label), 1.0f);
    gtk_box_pack_start(GTK_BOX(header_box), status_label, TRUE, TRUE, 0);
    set_status("Ready", "secondary");

    // ---- toolbar (theme picker removed - the app follows the system GTK theme) ----
    toolbar_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_start(toolbar_box, 15);
    gtk_widget_set_margin_end(toolbar_box, 15);
    gtk_widget_set_margin_top(toolbar_box, 2);
    gtk_box_pack_start(GTK_BOX(root_box), toolbar_box, FALSE, FALSE, 4);

    GtkWidget* settings_btn = gtk_menu_button_new();
    gtk_button_set_label(GTK_BUTTON(settings_btn), "SETTINGS");
    gtk_box_pack_end(GTK_BOX(toolbar_box), settings_btn, FALSE, FALSE, 0);

    // Packed after SETTINGS so it lands to its left (pack_end fills right to left).
    GtkWidget* install_apps_btn = gtk_button_new_with_label("INSTALL APPS");
    g_signal_connect(install_apps_btn, "clicked", G_CALLBACK(actions::run_exe_setup), nullptr);
    gtk_box_pack_end(GTK_BOX(toolbar_box), install_apps_btn, FALSE, FALSE, 0);
    gtk_widget_set_margin_end(install_apps_btn, 8);

    GtkWidget* settings_menu = gtk_menu_new();
    add_menu_item(settings_menu, "View Logs", actions::view_logs);
    gtk_menu_shell_append(GTK_MENU_SHELL(settings_menu), gtk_separator_menu_item_new());
    add_menu_item(settings_menu, "Prefix Configuration Manager...", actions::open_prefix_manager);
    add_menu_item(settings_menu, "GOG Library...", actions::open_gog_library);
    gtk_menu_shell_append(GTK_MENU_SHELL(settings_menu), gtk_separator_menu_item_new());
    add_menu_item(settings_menu, "Wine Configuration (winecfg)", actions::open_winecfg);
    add_menu_item(settings_menu, "Open Wine Prefix Folder", actions::open_wine_prefix_folder);
    add_menu_item(settings_menu, "Uninstall Program", actions::run_wine_uninstaller);
    add_menu_item(settings_menu, "Wine Explorer", actions::run_wine_explorer);
    // Everything about the runners - what is installed, downloading a new
    // build, unpacking an archive, opening the folder, removing - is one window.
    // Having a separate menu entry per action only spread it out again.
    add_menu_item(settings_menu, "Runner Options...", actions::open_runner_options_window);
    gtk_menu_shell_append(GTK_MENU_SHELL(settings_menu), gtk_separator_menu_item_new());
    add_menu_item(settings_menu, "Refresh List", actions::refresh_list);
    gtk_widget_show_all(settings_menu);
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(settings_btn), settings_menu);

    // ---- main content ----
    main_paned = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_widget_set_margin_start(GTK_WIDGET(main_paned), 15);
    gtk_widget_set_margin_end(GTK_WIDGET(main_paned), 15);
    gtk_widget_set_margin_bottom(GTK_WIDGET(main_paned), 15);
    gtk_box_pack_start(GTK_BOX(root_box), GTK_WIDGET(main_paned), TRUE, TRUE, 0);

    // ---- left panel (game list) ----
    left_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_end(left_panel, 10);
    gtk_paned_pack1(main_paned, left_panel, TRUE, FALSE);

    controls_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(left_panel), controls_box, FALSE, FALSE, 0);

    GtkWidget* sort_label = gtk_label_new("Sort:");
    gtk_box_pack_start(GTK_BOX(controls_box), sort_label, FALSE, FALSE, 0);

    sort_combo = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    gtk_combo_box_text_append_text(sort_combo, "A-Z");
    gtk_combo_box_text_append_text(sort_combo, "Z-A");
    gtk_combo_box_set_active(GTK_COMBO_BOX(sort_combo), 0);
    g_signal_connect(sort_combo, "changed", G_CALLBACK(actions::sort_by_selected), nullptr);
    gtk_box_pack_start(GTK_BOX(controls_box), GTK_WIDGET(sort_combo), FALSE, FALSE, 0);

    launch_label = gtk_label_new("Launch Mode:");
    gtk_widget_set_margin_start(launch_label, 15);
    gtk_box_pack_start(GTK_BOX(controls_box), launch_label, FALSE, FALSE, 0);

    launch_mode_combo = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    for (const char* mode : {"Normal", "GalliumHUD", "VulkanHUD", "MangoHud-GL", "Mangohud"}) {
        gtk_combo_box_text_append_text(launch_mode_combo, mode);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(launch_mode_combo), 0);
    gtk_box_pack_start(GTK_BOX(controls_box), GTK_WIDGET(launch_mode_combo), FALSE, FALSE, 0);

    GtkWidget* hud_config_btn = gtk_button_new_with_label("Config HUD");
    g_signal_connect(hud_config_btn, "clicked", G_CALLBACK(actions::config_hud), nullptr);
    gtk_widget_set_margin_start(hud_config_btn, 8);
    gtk_box_pack_start(GTK_BOX(controls_box), hud_config_btn, FALSE, FALSE, 0);

    // ---- tree view (game list) ----
    list_store = gtk_list_store_new(2, G_TYPE_INT, G_TYPE_STRING);
    tree = GTK_TREE_VIEW(gtk_tree_view_new_with_model(GTK_TREE_MODEL(list_store)));
    gtk_tree_view_set_headers_visible(tree, TRUE);

    GtkCellRenderer* no_renderer = gtk_cell_renderer_text_new();
    g_object_set(no_renderer, "xalign", 0.5, NULL);
    no_column =
        gtk_tree_view_column_new_with_attributes("No", no_renderer, "text", 0, NULL);
    gtk_tree_view_column_set_fixed_width(no_column, 40);
    gtk_tree_view_column_set_min_width(no_column, 40);
    gtk_tree_view_column_set_resizable(no_column, FALSE);
    gtk_tree_view_column_set_alignment(no_column, 0.5);
    gtk_tree_view_append_column(tree, no_column);

    GtkCellRenderer* name_renderer = gtk_cell_renderer_text_new();
    name_column =
        gtk_tree_view_column_new_with_attributes("GAME NAME", name_renderer, "text", 1, NULL);
    gtk_tree_view_column_set_expand(name_column, TRUE);
    gtk_tree_view_column_set_min_width(name_column, 200);
    gtk_tree_view_append_column(tree, name_column);

    GtkTreeSelection* selection = gtk_tree_view_get_selection(tree);
    gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);
    g_signal_connect(selection, "changed", G_CALLBACK(on_selection_changed), nullptr);
    g_signal_connect(tree, "button-press-event", G_CALLBACK(on_tree_button_press), nullptr);

    GtkWidget* tree_scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tree_scrolled), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(tree_scrolled), GTK_WIDGET(tree));
    gtk_box_pack_start(GTK_BOX(left_panel), tree_scrolled, TRUE, TRUE, 0);

    // ---- right panel (game details) ----
    right_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(right_panel), 15);
    gtk_paned_pack2(main_paned, right_panel, FALSE, FALSE);

    // Packed at the END (bottom) so it stays anchored there no matter how tall
    // the icon / title / info content becomes.
    button_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_end(GTK_BOX(right_panel), button_panel, FALSE, FALSE, 0);

    icon_frame = gtk_frame_new(nullptr);
    gtk_frame_set_shadow_type(GTK_FRAME(icon_frame), GTK_SHADOW_NONE);
    gtk_widget_set_size_request(icon_frame, cfg::ICON_WIDTH, cfg::ICON_HEIGHT);
    gtk_widget_set_halign(icon_frame, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(right_panel), icon_frame, FALSE, FALSE, 0);

    icon_image = gtk_image_new();
    gtk_container_add(GTK_CONTAINER(icon_frame), icon_image);

    game_title_label = gtk_label_new(nullptr);
    gtk_label_set_justify(GTK_LABEL(game_title_label), GTK_JUSTIFY_CENTER);
    gtk_label_set_line_wrap(GTK_LABEL(game_title_label), TRUE);
    set_game_title("No Game Selected");
    gtk_box_pack_start(GTK_BOX(right_panel), game_title_label, FALSE, FALSE, 0);

    info_label = gtk_label_new("Select a game to view details");
    gtk_label_set_justify(GTK_LABEL(info_label), GTK_JUSTIFY_LEFT);
    gtk_label_set_xalign(GTK_LABEL(info_label), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(info_label), TRUE);
    gtk_box_pack_start(GTK_BOX(right_panel), info_label, TRUE, TRUE, 0);

    // ---- button rows ----
    btn_row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_halign(btn_row1, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(button_panel), btn_row1, FALSE, FALSE, 0);

    GtkWidget* play_btn = gtk_button_new_with_label("\u25B6 PLAY");
    g_signal_connect(play_btn, "clicked", G_CALLBACK(actions::run_script), nullptr);
    gtk_box_pack_start(GTK_BOX(btn_row1), play_btn, FALSE, FALSE, 0);

    GtkWidget* add_btn = gtk_button_new_with_label("+ ADD");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(actions::add_script), nullptr);
    gtk_box_pack_start(GTK_BOX(btn_row1), add_btn, FALSE, FALSE, 0);

    GtkWidget* remove_btn = gtk_button_new_with_label("REMOVE");
    g_signal_connect(remove_btn, "clicked", G_CALLBACK(actions::remove_script), nullptr);
    gtk_box_pack_start(GTK_BOX(btn_row1), remove_btn, FALSE, FALSE, 0);

    btn_row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_halign(btn_row2, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(button_panel), btn_row2, FALSE, FALSE, 0);

    GtkWidget* rename_btn = gtk_button_new_with_label("RENAME");
    g_signal_connect(rename_btn, "clicked", G_CALLBACK(actions::rename_script), nullptr);
    gtk_box_pack_start(GTK_BOX(btn_row2), rename_btn, FALSE, FALSE, 0);

    GtkWidget* icon_btn = gtk_button_new_with_label("ICON");
    g_signal_connect(icon_btn, "clicked", G_CALLBACK(actions::change_icon), nullptr);
    gtk_box_pack_start(GTK_BOX(btn_row2), icon_btn, FALSE, FALSE, 0);

    GtkWidget* filemanager_btn = gtk_button_new_with_label("FOLDER");
    g_signal_connect(filemanager_btn, "clicked", G_CALLBACK(actions::open_file_manager), nullptr);
    gtk_box_pack_start(GTK_BOX(btn_row2), filemanager_btn, FALSE, FALSE, 0);

    // VIEW LOGS now lives in the SETTINGS menu and APPS SETUP became the
    // INSTALL APPS toolbar button, exactly like the Python launcher.

    // ---- final setup ----
    // Window modes (always starts windowed, maximized via the titlebar button,
    // F11 for fullscreen) plus the auto-scaling that follows them. Wired before
    // the window is shown, so the first size-allocate already uses the scaled
    // layout.
    winmode::set_scale_handler(&apply_scale);
    winmode::init(window);

    gtk_widget_show_all(GTK_WIDGET(window));
    g_idle_add(set_initial_paned_position, nullptr);
}

}  // namespace ui
