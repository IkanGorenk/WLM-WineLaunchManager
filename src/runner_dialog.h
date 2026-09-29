#pragma once

// The "Select Runner" dialog: Wine (Vanilla) / Proton GE / Proton-CachyOS,
// including prefix location controls and launch options / comment fields.

#include <optional>
#include <string>

#include "scripts.h"

namespace runner_dialog {

// purpose: "play" or "setup" (only affects the dialog title).
// Returns the chosen runner, or nullopt when cancelled.
std::optional<scripts::RunnerChoice> ask_runner_choice(const std::string& parent_script_name,
                                                       const std::string& purpose);

}  // namespace runner_dialog
