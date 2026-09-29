// Wine Launch Manager - GTK3 launcher, C++ port of launcher_gtk3.py

#include <gtk/gtk.h>

#include <cstdio>
#include <cstring>

#include "config.h"
#include "download.h"
#include "live_log.h"
#include "ui.h"

namespace {

// Python: _print_version_and_exit() - handled before gtk_init so it works on a
// machine without a display.
bool print_version_if_asked(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            std::printf("WLM Version: %s\n", cfg::WLM_VERSION);
            std::printf("Developer: %s\n", cfg::WLM_DEVELOPER);
            std::printf("Maintener: %s\n", cfg::WLM_MAINTAINER);
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (print_version_if_asked(argc, argv)) return 0;

    // WebKitGTK renders the embedded GOG sign-in page through DMABUF/GBM, which
    // needs a real DRM device. On the proprietary NVIDIA driver (especially the
    // legacy branch, without nvidia-drm.modeset=1) that fails with
    // "KMS: DRM_IOCTL_MODE_CREATE_DUMB failed: Permission denied" and
    // "Failed to create GBM buffer", leaving the login page blank.
    //
    // WEBKIT_DISABLE_DMABUF_RENDERER=1 makes WebKit fall back to its software
    // renderer. It only affects that one embedded page; the launcher's own
    // windows and the games it starts are untouched.
    //
    // The 0 is "overwrite = false": a user who sets this in the environment
    // (to 0) keeps the GPU renderer.
    g_setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", FALSE);

    // Same order as the Python version: create the ~/wlm folders first,
    // then bring up GTK and build the window from the saved config.
    cfg::ensure_directories();
    dload::ensure_env_config();

    gtk_init(&argc, &argv);

    // The window always opens at the standard windowed size; nothing is
    // remembered between runs.
    ui::build_window();
    ui::update_script_list();

    // Background poller feeding live game output into any open Logs window.
    g_timeout_add(150, live_log::poll_queues, nullptr);

    gtk_main();
    return 0;
}
