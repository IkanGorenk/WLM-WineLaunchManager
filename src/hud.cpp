#include "hud.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "config.h"
#include "ui.h"
#include "window_mode.h"

namespace hud {

namespace {

namespace fs = std::filesystem;

// Which indicators each launch mode shows. key -> label, laid out in two
// columns like the Python dialog.
struct Metric {
    const char* key;
    const char* description;
};

const Metric VULKAN_HUD_METRICS[] = {
    {"fps", "FPS"},
    {"frametimes", "Frame time graph"},
    {"gpuload", "GPU load"},
    {"memory", "Memory usage"},
    {"devinfo", "Device info (GPU/driver)"},
    {"version", "DXVK version"},
    {"api", "D3D API version"},
    {"drawcalls", "Draw calls"},
    {"submissions", "Command submissions"},
    {"pipelines", "Pipeline compiles"},
    {"compiler", "Shader compiler activity"},
    {"samplers", "Sampler count"},
};

const std::vector<std::string> VULKAN_HUD_SCALE_OPTIONS = {"0.5", "0.75", "1", "1.25",
                                                           "1.5", "2",   "3"};

const Metric GALLIUM_HUD_METRICS[] = {
    {"fps", "FPS"},
    {"cpu", "Overall CPU load"},
    {"GPU-load", "GPU load (RADV/radeonsi)"},
    {"VRAM-usage", "VRAM usage"},
    {"GTT-usage", "GTT (system) memory usage"},
    {"draw-calls", "Draw calls"},
    {"requested-VRAM", "Requested VRAM"},
    {"requested-GTT", "Requested GTT memory"},
};

const std::vector<std::string> GALLIUM_HUD_SCALE_OPTIONS = {"1", "2", "3", "4", "5"};

Config default_config() {
    Config cfg;
    cfg.vulkan.metrics = {"fps"};
    cfg.vulkan.scale = "1";
    cfg.gallium.metrics = {"fps", "cpu", "GPU-load"};
    cfg.gallium.scale = "1";
    return cfg;
}

// hud_config.json is read raw (not normalised) so fields the launcher does not
// own - notably "window_position" - survive a metrics/scale update.
json read_raw() {
    json raw;
    cfg::read_json(cfg::hud_config_file, raw);  // returns an empty object on any problem
    return raw;
}

Section section_from_json(const json& value, const Section& fallback) {
    Section section = fallback;
    if (!value.is_object()) return section;

    if (value.contains("metrics") && value["metrics"].is_array()) {
        std::vector<std::string> metrics;
        for (const json& item : value["metrics"]) {
            if (item.is_string()) metrics.push_back(item.get<std::string>());
        }
        section.metrics = metrics;
    }
    const std::string scale = json_str(value, "scale");
    if (!scale.empty()) section.scale = scale;
    return section;
}

json section_to_json(const Section& section) {
    json value = json::object();
    json metrics = json::array();
    for (const std::string& metric : section.metrics) {
        metrics.push_back(metric);
    }
    value["metrics"] = metrics;
    value["scale"] = section.scale;
    return value;
}

bool contains(const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

// ---- dialog state (freed when the window is destroyed) ----
struct SectionWidgets {
    std::vector<std::pair<std::string, GtkWidget*>> checks;
    GtkWidget* scale_combo = nullptr;
};

struct DialogState {
    GtkWidget* window = nullptr;
    SectionWidgets vulkan;
    SectionWidgets gallium;
    int last_x = -1;
    int last_y = -1;
};

// GClosureNotify for g_signal_connect_data: frees the state once the window's
// signal handlers are torn down (i.e. when the dialog is really destroyed).
void free_state_closure(gpointer data, GClosure*) { delete static_cast<DialogState*>(data); }

std::string active_scale(GtkWidget* combo) {
    gchar* text = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    std::string value = text ? text : "1";
    g_free(text);
    return value;
}

void set_scale(GtkWidget* combo, const std::vector<std::string>& options,
               const std::string& value) {
    // Mirror Python's `value if value in options else options[2]`.
    std::string wanted = contains(options, value) ? value : options[2];
    for (size_t i = 0; i < options.size(); ++i) {
        if (options[i] == wanted) {
            gtk_combo_box_set_active(GTK_COMBO_BOX(combo), static_cast<gint>(i));
            return;
        }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 2);
}

void collect_metrics(const SectionWidgets& widgets, std::vector<std::string>* out) {
    for (const auto& entry : widgets.checks) {
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(entry.second))) {
            out->push_back(entry.first);
        }
    }
}

// Persist only the position - the Python dialog remembers where it was moved
// to even when the settings themselves were not saved.
void persist_position(DialogState* state) {
    if (state->last_x < 0 || state->last_y < 0) return;
    char text[32];
    std::snprintf(text, sizeof(text), "+%d+%d", state->last_x, state->last_y);
    json raw = read_raw();
    raw["window_position"] = text;
    cfg::write_json(cfg::hud_config_file, raw);
}

std::optional<std::pair<int, int>> load_window_position() {
    json raw = read_raw();
    if (!raw.contains("window_position") || !raw["window_position"].is_string()) return std::nullopt;
    const std::string text = raw["window_position"].get<std::string>();
    if (text.empty() || text[0] != '+') return std::nullopt;
    size_t second = text.find('+', 1);
    if (second == std::string::npos || text.find('+', second + 1) != std::string::npos) {
        return std::nullopt;
    }
    try {
        int x = std::stoi(text.substr(1, second - 1));
        int y = std::stoi(text.substr(second + 1));
        return std::make_pair(x, y);
    } catch (...) {
        return std::nullopt;
    }
}

void workarea_size(int* width, int* height) {
    *width = 0;
    *height = 0;
    GdkDisplay* display = gdk_display_get_default();
    if (!display) return;
    GdkMonitor* monitor = gdk_display_get_primary_monitor(display);
    if (!monitor) monitor = gdk_display_get_monitor(display, 0);
    if (!monitor) return;
    GdkRectangle geometry;
    gdk_monitor_get_geometry(monitor, &geometry);
    *width = geometry.width;
    *height = geometry.height;
}

// ---- one checklist section (title + 2-column grid + size selector) ----
void build_section(GtkWidget* frame, const char* title_text, const Metric* metrics,
                   size_t metric_count, const std::vector<std::string>& scale_options,
                   const Section& saved, SectionWidgets* out) {
    GtkWidget* section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_bottom(section, winmode::px(14));
    gtk_box_pack_start(GTK_BOX(frame), section, FALSE, FALSE, 0);

    GtkWidget* title = gtk_label_new(nullptr);
    gchar* bold = g_markup_escape_text(title_text, -1);
    std::string markup = std::string("<b>") + bold + "</b>";
    g_free(bold);
    gtk_label_set_markup(GTK_LABEL(title), markup.c_str());
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_box_pack_start(GTK_BOX(section), title, FALSE, FALSE, 0);

    GtkWidget* grid = gtk_grid_new();
    gtk_widget_set_margin_top(grid, winmode::px(6));
    gtk_grid_set_column_spacing(GTK_GRID(grid), winmode::px(20));
    gtk_grid_set_row_spacing(GTK_GRID(grid), winmode::px(2));
    gtk_box_pack_start(GTK_BOX(section), grid, FALSE, FALSE, 0);

    constexpr int COLUMNS = 2;
    for (size_t i = 0; i < metric_count; ++i) {
        std::string label = std::string(metrics[i].key) + " - " + metrics[i].description;
        GtkWidget* check = gtk_check_button_new_with_label(label.c_str());
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check),
                                     contains(saved.metrics, metrics[i].key));
        gtk_widget_set_halign(check, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), check, static_cast<int>(i) % COLUMNS,
                        static_cast<int>(i) / COLUMNS, 1, 1);
        out->checks.emplace_back(metrics[i].key, check);
    }

    GtkWidget* size_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, winmode::px(6));
    gtk_widget_set_margin_top(size_row, winmode::px(8));
    gtk_box_pack_start(GTK_BOX(section), size_row, FALSE, FALSE, 0);

    GtkWidget* size_label = gtk_label_new("HUD Size:");
    gtk_box_pack_start(GTK_BOX(size_row), size_label, FALSE, FALSE, 0);

    GtkWidget* combo = gtk_combo_box_text_new();
    for (const std::string& option : scale_options) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), option.c_str());
    }
    gtk_combo_box_set_wrap_width(GTK_COMBO_BOX(combo), 1);
    gtk_widget_set_size_request(combo, winmode::px(70), -1);
    set_scale(combo, scale_options, saved.scale);
    gtk_box_pack_start(GTK_BOX(size_row), combo, FALSE, FALSE, 0);

    GtkWidget* hint = gtk_label_new("(1 = default)");
    gtk_style_context_add_class(gtk_widget_get_style_context(hint), "dim-label");
    gtk_box_pack_start(GTK_BOX(size_row), hint, FALSE, FALSE, 0);

    out->scale_combo = combo;
}

void on_save(GtkButton*, gpointer data) {
    auto* state = static_cast<DialogState*>(data);
    Config saved = default_config();

    saved.vulkan.metrics.clear();
    collect_metrics(state->vulkan, &saved.vulkan.metrics);
    saved.vulkan.scale = active_scale(state->vulkan.scale_combo);

    saved.gallium.metrics.clear();
    collect_metrics(state->gallium, &saved.gallium.metrics);
    saved.gallium.scale = active_scale(state->gallium.scale_combo);

    // Keep the stored window position (Python: save_hud_config()).
    json raw = read_raw();
    raw["vulkan_hud"] = section_to_json(saved.vulkan);
    raw["gallium_hud"] = section_to_json(saved.gallium);
    cfg::write_json(cfg::hud_config_file, raw);
    persist_position(state);

    if (ui::status_label) ui::set_status("HUD configuration saved.", "success");
}

void on_reset(GtkButton*, gpointer data) {
    auto* state = static_cast<DialogState*>(data);
    Config defaults = default_config();

    auto apply = [&](SectionWidgets* widgets, const Section& values,
                     const std::vector<std::string>& options) {
        for (const auto& entry : widgets->checks) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(entry.second),
                                         contains(values.metrics, entry.first));
        }
        set_scale(widgets->scale_combo, options, values.scale);
    };
    apply(&state->vulkan, defaults.vulkan, VULKAN_HUD_SCALE_OPTIONS);
    apply(&state->gallium, defaults.gallium, GALLIUM_HUD_SCALE_OPTIONS);
}

void on_close(GtkButton*, gpointer data) {
    auto* state = static_cast<DialogState*>(data);
    persist_position(state);
    if (state->window) gtk_widget_destroy(state->window);
}

gboolean on_delete_event(GtkWidget* widget, GdkEvent*, gpointer data) {
    auto* state = static_cast<DialogState*>(data);
    persist_position(state);
    (void)widget;
    return FALSE;  // let the window close normally
}

gboolean on_configure_event(GtkWidget* widget, GdkEvent* event, gpointer data) {
    auto* state = static_cast<DialogState*>(data);
    auto* real_event = reinterpret_cast<GdkEventConfigure*>(event);
    state->last_x = real_event->x;
    state->last_y = real_event->y;
    (void)widget;
    return FALSE;
}

}  // namespace

Config load() {
    Config cfg = default_config();
    json raw;
    if (!cfg::read_json(cfg::hud_config_file, raw)) return cfg;

    cfg.vulkan = section_from_json(json_obj(raw, "vulkan_hud"), cfg.vulkan);
    cfg.gallium = section_from_json(json_obj(raw, "gallium_hud"), cfg.gallium);
    return cfg;
}

void save(const Config& new_cfg) {
    json raw = read_raw();
    raw["vulkan_hud"] = section_to_json(new_cfg.vulkan);
    raw["gallium_hud"] = section_to_json(new_cfg.gallium);
    cfg::write_json(cfg::hud_config_file, raw);
}

std::string vulkan_env_prefix() {
    Config cfg = load();
    std::vector<std::string> metrics = cfg.vulkan.metrics;
    if (metrics.empty()) metrics = {"fps"};

    std::string joined;
    for (const std::string& metric : metrics) {
        if (!joined.empty()) joined += ",";
        joined += metric;
    }
    if (cfg.vulkan.scale != "1") joined += "scale=" + cfg.vulkan.scale;
    return "DXVK_HUD=" + joined;
}

std::string gallium_env_prefix() {
    Config cfg = load();
    std::vector<std::string> metrics = cfg.gallium.metrics;
    if (metrics.empty()) metrics = {"fps"};

    std::string joined;
    for (const std::string& metric : metrics) {
        if (!joined.empty()) joined += "+";
        joined += metric;
    }
    std::string env_str = "GALLIUM_HUD=" + joined;
    if (cfg.gallium.scale != "1") env_str += " GALLIUM_HUD_SCALE=" + cfg.gallium.scale;
    return env_str;
}

void open_config_dialog() {
    Config cfg = load();

    auto* state = new DialogState();

    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Config HUD");
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_transient_for(GTK_WINDOW(window), ui::window);
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    state->window = window;

    GtkWidget* frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(frame), winmode::px(15));
    gtk_container_add(GTK_CONTAINER(window), frame);

    GtkWidget* intro = gtk_label_new(
        "Choose what each HUD mode shows, and its size.\n"
        "Applies to the \"VulkanHUD\" and \"GalliumHUD\" Launch Mode options.");
    gtk_label_set_xalign(GTK_LABEL(intro), 0.0f);
    gtk_label_set_justify(GTK_LABEL(intro), GTK_JUSTIFY_LEFT);
    gtk_style_context_add_class(gtk_widget_get_style_context(intro), "dim-label");
    gtk_widget_set_margin_bottom(intro, winmode::px(12));
    gtk_box_pack_start(GTK_BOX(frame), intro, FALSE, FALSE, 0);

    build_section(frame, "Vulkan HUD (DXVK_HUD) - DirectX 9-11 games via Vulkan",
                  VULKAN_HUD_METRICS,
                  sizeof(VULKAN_HUD_METRICS) / sizeof(VULKAN_HUD_METRICS[0]),
                  VULKAN_HUD_SCALE_OPTIONS, cfg.vulkan, &state->vulkan);

    GtkWidget* separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_bottom(separator, winmode::px(14));
    gtk_box_pack_start(GTK_BOX(frame), separator, FALSE, FALSE, 0);

    build_section(frame, "Gallium HUD (GALLIUM_HUD) - native OpenGL games via Mesa",
                  GALLIUM_HUD_METRICS,
                  sizeof(GALLIUM_HUD_METRICS) / sizeof(GALLIUM_HUD_METRICS[0]),
                  GALLIUM_HUD_SCALE_OPTIONS, cfg.gallium, &state->gallium);

    GtkWidget* btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, winmode::px(4));
    gtk_widget_set_margin_top(btn_row, winmode::px(4));
    gtk_widget_set_halign(btn_row, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(frame), btn_row, FALSE, FALSE, 0);

    GtkWidget* save_btn = gtk_button_new_with_label("Save");
    gtk_widget_set_size_request(save_btn, winmode::px(110), -1);
    g_signal_connect_data(save_btn, "clicked", G_CALLBACK(on_save), state, nullptr,
                          G_CONNECT_DEFAULT);
    gtk_box_pack_start(GTK_BOX(btn_row), save_btn, FALSE, FALSE, 0);

    GtkWidget* reset_btn = gtk_button_new_with_label("Reset to Defaults");
    gtk_widget_set_size_request(reset_btn, winmode::px(150), -1);
    g_signal_connect_data(reset_btn, "clicked", G_CALLBACK(on_reset), state, nullptr,
                          G_CONNECT_DEFAULT);
    gtk_box_pack_start(GTK_BOX(btn_row), reset_btn, FALSE, FALSE, 0);

    GtkWidget* close_btn = gtk_button_new_with_label("Close");
    gtk_widget_set_size_request(close_btn, winmode::px(100), -1);
    g_signal_connect_data(close_btn, "clicked", G_CALLBACK(on_close), state, nullptr,
                          G_CONNECT_DEFAULT);
    gtk_box_pack_start(GTK_BOX(btn_row), close_btn, FALSE, FALSE, 0);

    g_signal_connect_data(window, "delete-event", G_CALLBACK(on_delete_event), state,
                          free_state_closure, G_CONNECT_DEFAULT);
    g_signal_connect(window, "configure-event", G_CALLBACK(on_configure_event), state);

    gtk_widget_show_all(window);

    // Restore the last position (clamped to the current screen so a dialog
    // remembered on a bigger monitor cannot open off-screen), otherwise centre
    // it over the main window - same rules as the Python dialog.
    int screen_w = 0;
    int screen_h = 0;
    workarea_size(&screen_w, &screen_h);

    int width = 0;
    int height = 0;
    gtk_window_get_size(GTK_WINDOW(window), &width, &height);

    int x = -1;
    int y = -1;
    std::optional<std::pair<int, int>> saved_pos = load_window_position();
    if (saved_pos) {
        x = std::min(std::max(saved_pos->first, 0), std::max(screen_w - width, 0));
        y = std::min(std::max(saved_pos->second, 0), std::max(screen_h - height, 0));
    } else if (ui::window) {
        gint root_x = 0;
        gint root_y = 0;
        gint root_w = 0;
        gint root_h = 0;
        gtk_window_get_position(ui::window, &root_x, &root_y);
        gtk_window_get_size(ui::window, &root_w, &root_h);
        x = root_x + (root_w - width) / 2;
        y = root_y + (root_h - height) / 2;
    }
    if (x < 0 || y < 0) {
        x = std::max((screen_w - width) / 2, 0);
        y = std::max((screen_h - height) / 2, 0);
    }
    gtk_window_move(GTK_WINDOW(window), x, y);
    state->last_x = x;
    state->last_y = y;
    gtk_window_present(GTK_WINDOW(window));
}

}  // namespace hud
