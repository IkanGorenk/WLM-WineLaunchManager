#pragma once

// Window sizing and automatic UI scaling.
//
// The launcher always opens as a normal (windowed) window at the standard
// design size, shrunk only if the display is too small to show it. Nothing is
// remembered between runs - no saved size, no saved position - so every start
// looks the same.
//
// Maximize/unmaximize is left entirely to GTK and the window manager. Nothing
// here fights it, which is the point: an earlier version imposed a minimum size
// as large as the design height, and on a display whose usable height is
// smaller than that minimum the window manager cannot grant it, so it silently
// refused both the maximize and the unmaximize request and the titlebar button
// looked dead. The minimum this module sets is therefore always clamped to what
// the display can actually show, which is what makes the button work.
//
// Scaling works in two layers:
//   * text: a screen-wide CSS provider (`* { font-size: ... }`) so every window
//     the app opens - main window, dialogs, menus - grows together,
//   * geometry: the widgets in ui.cpp (icon, paddings, spacings, column widths)
//     are re-applied through px() by a handler registered with
//     set_scale_handler().
//
// The factor never exceeds the real width/height ratio, so a scaled layout can
// never ask for more space than the window actually has.

#include <gtk/gtk.h>

namespace winmode {

// The size the layout was authored for; scale is 1.0 there.
constexpr int DESIGN_W = 1000;
constexpr int DESIGN_H = 720;

// Upper bound for the auto-scale (a very large monitor shouldn't turn into a
// poster: text grows, but stays readable/dense enough to be useful).
constexpr double MAX_SCALE = 2.5;

// Current auto-scale factor (1.0 while the window is at its design size).
double scale();

// Scale a design-time pixel value with the current factor.
int px(int design_px);

// Callback ui.cpp registers to re-apply scaled spacing/sizes.
using ScaleHandler = void (*)(double);
void set_scale_handler(ScaleHandler handler);

// The standard windowed size: the design size, shrunk to what the display can
// actually show (minus the window decorations) when the display is smaller than
// the design size. Call before the window is shown.
void standard_size(int* width, int* height);

// Installs F11 fullscreen and auto-scaling on resize/maximize/fullscreen. Call
// once, after the main window is built.
void init(GtkWindow* window);

}  // namespace winmode
