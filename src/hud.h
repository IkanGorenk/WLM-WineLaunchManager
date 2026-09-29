#pragma once

// "Config HUD" feature: which indicators the VulkanHUD / GalliumHUD launch
// modes show, and how large they are (~/wlm/hud_config.json).
// C++ port of load_hud_config()/save_hud_config()/build_*_hud_env_prefix()/
// open_hud_config_dialog() in launcher.py.

#include <gtk/gtk.h>

#include <string>
#include <vector>

namespace hud {

struct Section {
    std::vector<std::string> metrics;
    std::string scale;
};

struct Config {
    Section vulkan;   // -> DXVK_HUD
    Section gallium;  // -> GALLIUM_HUD / GALLIUM_HUD_SCALE
};

// Reads hud_config.json, filling in every missing/broken field from the
// defaults, so the result is always complete.
Config load();

// Saves metrics/scale without dropping the stored dialog position.
void save(const Config& cfg);

// "DXVK_HUD=fps,scale=2" / "GALLIUM_HUD=fps+cpu GALLIUM_HUD_SCALE=2",
// ready to be pasted in front of the launch command (see actions::run_script).
std::string vulkan_env_prefix();
std::string gallium_env_prefix();

// The "Config HUD" dialog (checkbox lists + size combos for both sections).
void open_config_dialog();

}  // namespace hud
