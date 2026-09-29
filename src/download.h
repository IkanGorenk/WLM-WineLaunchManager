#pragma once

// "Download ... Online" feature: fetch the Proton GE / Proton-CachyOS release
// lists from the GitHub API and download + extract a chosen build.
// The URLs live in ~/wlm/env_config.yaml (created with the built-in defaults
// on first start) so users can point them at a mirror.
// C++ port of fetch_*_releases()/download_and_install_*_worker()/
// open_*_download_dialog()/load_env_config() in launcher.py.

#include <gtk/gtk.h>

namespace dload {

// Writes ~/wlm/env_config.yaml with the default URLs when it does not exist
// yet. Called once at startup, like load_env_config() does on Python import.
void ensure_env_config();

// Settings -> "Download ProtonGE Online..." / "Download Proton-CachyOS Online..."
void open_protonge_download_dialog(GtkWindow* parent = nullptr);
void open_protoncachyos_download_dialog(GtkWindow* parent = nullptr);

}  // namespace dload
