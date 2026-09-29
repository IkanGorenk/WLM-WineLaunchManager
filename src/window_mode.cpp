#include "window_mode.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace winmode {
namespace {

GtkWindow* g_window = nullptr;
ScaleHandler g_handler = nullptr;
double g_scale = 1.0;

bool g_fullscreen = false;
guint g_rescale_source = 0;

// Font metrics of the untouched theme, measured once before any of our CSS is
// applied - the scaled font-size is a multiple of this.
double g_base_font_px = 0.0;
GtkCssProvider* g_provider = nullptr;

// Space the window manager needs for the title bar and borders, so a size that
// is clamped to the work area still fits inside the actual screen.
constexpr int DECO_W = 24;
constexpr int DECO_H = 48;

// Never shrink below this, however small the display is - a window this small
// is unusable, and the window manager would fight us over it.
constexpr int MIN_W = 640;
constexpr int MIN_H = 480;

double measure_base_font_px() {
    if (!g_window) return 13.0;
    PangoContext* pango_ctx = gtk_widget_get_pango_context(GTK_WIDGET(g_window));
    const PangoFontDescription* desc = pango_ctx ? pango_context_get_font_description(pango_ctx)
                                                 : nullptr;
    int points = desc ? pango_font_description_get_size(desc) / PANGO_SCALE : 0;
    if (points <= 0) points = 10;  // sensible fallback, matches gtk-font-name

    // GTK's CSS px units are resolved against this dpi (gtk-xft-dpi is in
    // 1024ths), so convert the point size exactly the way GTK does.
    gint dpi_1024 = 96 * 1024;
    g_object_get(gtk_widget_get_settings(GTK_WIDGET(g_window)), "gtk-xft-dpi", &dpi_1024, nullptr);
    double dpi = dpi_1024 > 0 ? dpi_1024 / 1024.0 : 96.0;
    return points * dpi / 72.0;
}

void apply_font_scale() {
    if (!g_window) return;
    if (g_base_font_px <= 0.0) g_base_font_px = measure_base_font_px();

    if (!g_provider) {
        g_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                                  GTK_STYLE_PROVIDER(g_provider),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

    if (g_scale <= 1.0) {
        // Design size: hand the widgets back to the theme untouched, so the
        // default windowed look stays what it always was.
        gtk_css_provider_load_from_data(g_provider, "", -1, nullptr);
        return;
    }

    char css[160];
    std::snprintf(css, sizeof(css), "* { font-size: %.2fpx; }", g_base_font_px * g_scale);
    GError* error = nullptr;
    gtk_css_provider_load_from_data(g_provider, css, -1, &error);
    if (error) {
        g_warning("ui scale css: %s", error->message);
        g_error_free(error);
    }
}

// Derived from the live window size: whichever dimension is tighter wins, and
// rounding DOWN keeps the layout from ever asking for more space than exists.
double target_scale() {
    if (!g_window) return 1.0;
    int w = gtk_widget_get_allocated_width(GTK_WIDGET(g_window));
    int h = gtk_widget_get_allocated_height(GTK_WIDGET(g_window));
    if (w <= 1 || h <= 1) return g_scale;

    double s = std::min(w / static_cast<double>(DESIGN_W), h / static_cast<double>(DESIGN_H));
    s = std::clamp(s, 1.0, MAX_SCALE);
    s = std::floor(s * 20.0) / 20.0;  // 0.05 steps, rounded down (no overflow)
    return std::max(1.0, s);
}

void rescale_now() {
    double s = target_scale();
    if (s == g_scale) return;
    g_scale = s;
    apply_font_scale();
    if (g_handler) g_handler(g_scale);
}

gboolean rescale_cb(gpointer) {
    g_rescale_source = 0;
    if (!g_window || gtk_widget_in_destruction(GTK_WIDGET(g_window))) return G_SOURCE_REMOVE;
    rescale_now();
    return G_SOURCE_REMOVE;
}

// Rescaling on every configure-event would make dragging the window flicker, so
// changes are applied once resizing settles.
void schedule_rescale() {
    if (g_rescale_source) g_source_remove(g_rescale_source);
    g_rescale_source = g_timeout_add(150, rescale_cb, nullptr);
}

gboolean on_configure(GtkWidget*, GdkEventConfigure*, gpointer) {
    // Purely a "the size changed" signal: the auto-scale follows it, and
    // maximizing/unmaximizing is the window manager's business, not ours.
    schedule_rescale();
    return GDK_EVENT_PROPAGATE;
}

gboolean on_window_state(GtkWidget*, GdkEventWindowState* event, gpointer) {
    g_fullscreen = (event->new_window_state & GDK_WINDOW_STATE_FULLSCREEN) != 0;
    schedule_rescale();
    return GDK_EVENT_PROPAGATE;
}

// F11 is the standard fullscreen shortcut; a fullscreen action issued by the
// WM itself arrives as a state change and lands in on_window_state() instead.
gboolean on_key_press(GtkWidget*, GdkEventKey* event, gpointer) {
    if (event->keyval != GDK_KEY_F11 || !g_window) return GDK_EVENT_PROPAGATE;
    if (g_fullscreen) {
        gtk_window_unfullscreen(g_window);
    } else {
        gtk_window_fullscreen(g_window);
    }
    return GDK_EVENT_STOP;
}

}  // namespace

double scale() {
    return g_scale;
}

int px(int design_px) {
    return static_cast<int>(std::lround(design_px * g_scale));
}

void set_scale_handler(ScaleHandler handler) {
    g_handler = handler;
}

void standard_size(int* width, int* height) {
    int w = DESIGN_W;
    int h = DESIGN_H;

    // The work area already excludes panels and docks; the decorations are on
    // top of that, so subtract them as well. A window that is only just as big
    // as the work area would push its own title bar off the screen.
    GdkRectangle work{0, 0, 0, 0};
    GdkDisplay* display = gdk_display_get_default();
    if (display != nullptr) {
        GdkMonitor* monitor = gdk_display_get_primary_monitor(display);
        if (monitor == nullptr) monitor = gdk_display_get_monitor(display, 0);
        if (monitor != nullptr) gdk_monitor_get_workarea(monitor, &work);
    }
    if (work.width > 0 && work.height > 0) {
        w = std::min(w, work.width - DECO_W);
        h = std::min(h, work.height - DECO_H);
        w = std::clamp(w, MIN_W, DESIGN_W);
        h = std::clamp(h, MIN_H, DESIGN_H);
    }

    *width = w;
    *height = h;
}

void init(GtkWindow* window) {
    if (g_window) return;  // already wired up
    g_window = window;
    g_signal_connect(window, "configure-event", G_CALLBACK(on_configure), nullptr);
    g_signal_connect(window, "window-state-event", G_CALLBACK(on_window_state), nullptr);
    g_signal_connect(window, "key-press-event", G_CALLBACK(on_key_press), nullptr);
}

}  // namespace winmode
