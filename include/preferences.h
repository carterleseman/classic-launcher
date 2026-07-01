#pragma once

#include <string>
#include <vector>

// Path to the game client's preferences file: {install_dir}\Bin\preferences.xml
std::string preferences_path(const std::string& install_dir);

// Unique WxH resolution strings from the primary monitor, largest first.
std::vector<std::string> enumerate_monitor_resolutions();

// All resolution dropdown options in display order:
// borderless, fullscreen, then monitor resolutions.
std::vector<std::string> resolution_dropdown_options();

// Returns true if selection is a valid dropdown value (borderless, fullscreen,
// or a resolution present in the current monitor list).
bool is_valid_resolution_selection(const std::string& selection);

// Patch VideoSettings in preferences.xml before launch.
// No-op when selection is "off".
void apply_video_settings(const std::string& install_dir,
                          const std::string& selection);
