#include "preferences.h"
#include "logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

static constexpr int kMinResolutionWidth  = 800;
static constexpr int kMinResolutionHeight = 600;

static constexpr int kFullscreenWindowed   = 0;
static constexpr int kFullscreenExclusive  = 1;
static constexpr int kFullscreenBorderless = 2;

static constexpr const char* kSelectionOff         = "off";
static constexpr const char* kSelectionBorderless  = "borderless";
static constexpr const char* kSelectionFullscreen  = "fullscreen";

// ---------------------------------------------------------------------------

std::string preferences_path(const std::string& install_dir)
{
    return install_dir + "\\Bin\\preferences.xml";
}

// ---------------------------------------------------------------------------

std::vector<std::string> enumerate_monitor_resolutions()
{
    struct Mode {
        int width;
        int height;
    };

    std::set<std::pair<int, int>> seen;
    std::vector<Mode> modes;

    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);

    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); ++i) {
        if (dm.dmPelsWidth == 0 || dm.dmPelsHeight == 0)
            continue;
        const int w = static_cast<int>(dm.dmPelsWidth);
        const int h = static_cast<int>(dm.dmPelsHeight);
        if (w < kMinResolutionWidth || h < kMinResolutionHeight)
            continue;
        auto key = std::make_pair(w, h);
        if (!seen.insert(key).second)
            continue;
        modes.push_back({ key.first, key.second });
    }

    std::sort(modes.begin(), modes.end(),
              [](const Mode& a, const Mode& b) {
                  const int64_t pa = static_cast<int64_t>(a.width) * a.height;
                  const int64_t pb = static_cast<int64_t>(b.width) * b.height;
                  if (pa != pb) return pa > pb;
                  if (a.width != b.width) return a.width > b.width;
                  return a.height > b.height;
              });

    std::vector<std::string> out;
    out.reserve(modes.size());
    for (const Mode& m : modes) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%dx%d", m.width, m.height);
        out.emplace_back(buf);
    }
    return out;
}

// ---------------------------------------------------------------------------

std::vector<std::string> resolution_dropdown_options()
{
    std::vector<std::string> out;
    out.push_back(kSelectionOff);
    out.push_back(kSelectionBorderless);
    out.push_back(kSelectionFullscreen);
    auto monitor = enumerate_monitor_resolutions();
    out.insert(out.end(), monitor.begin(), monitor.end());
    return out;
}

// ---------------------------------------------------------------------------

bool is_valid_resolution_selection(const std::string& selection)
{
    if (selection == kSelectionOff
        || selection == kSelectionBorderless
        || selection == kSelectionFullscreen)
        return true;

    for (const std::string& r : enumerate_monitor_resolutions()) {
        if (r == selection)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
//  XML helpers
// ---------------------------------------------------------------------------
static bool replace_element_value(std::string& xml,
                                  const char* element_name,
                                  const std::string& new_value)
{
    const std::string open = std::string("<") + element_name;
    size_t pos = xml.find(open);
    if (pos == std::string::npos)
        return false;

    size_t gt = xml.find('>', pos);
    if (gt == std::string::npos)
        return false;

    const std::string close_tag = std::string("</") + element_name + ">";
    size_t close = xml.find(close_tag, gt);
    if (close == std::string::npos)
        return false;

    xml.replace(gt + 1, close - gt - 1, new_value);
    return true;
}

static bool read_text_file(const std::string& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static bool write_text_file_atomic(const std::string& path, const std::string& content)
{
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        f << content;
        if (!f)
            return false;
    }

    if (!MoveFileExA(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileA(tmp.c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------

void apply_video_settings(const std::string& install_dir,
                          const std::string& selection)
{
    if (selection == kSelectionOff) return;

    const std::string path = preferences_path(install_dir);

    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LOG_WARN("PREFS", "preferences.xml not found: " + path);
        return;
    }

    std::string xml;
    if (!read_text_file(path, xml)) {
        LOG_ERR("PREFS", "Could not read " + path);
        return;
    }

    int fullscreen_value = kFullscreenBorderless;
    bool set_resolution = false;
    std::string resolution_value;

    if (selection == kSelectionBorderless) {
        fullscreen_value = kFullscreenBorderless;
    } else if (selection == kSelectionFullscreen) {
        fullscreen_value = kFullscreenExclusive;
    } else {
        fullscreen_value = kFullscreenWindowed;
        resolution_value = selection;
        set_resolution = true;
    }

    if (!replace_element_value(xml, "IsFullscreen", std::to_string(fullscreen_value))) {
        LOG_ERR("PREFS", "IsFullscreen tag not found in " + path);
        return;
    }

    if (set_resolution) {
        if (!replace_element_value(xml, "Resolution", resolution_value)) {
            LOG_ERR("PREFS", "Resolution tag not found in " + path);
            return;
        }
    }

    if (!write_text_file_atomic(path, xml)) {
        LOG_ERR("PREFS", "Could not write " + path);
        return;
    }

    LOG_STAT("PREFS", "Applied video settings: selection=" + selection
             + " IsFullscreen=" + std::to_string(fullscreen_value)
             + (set_resolution ? " Resolution=" + resolution_value : ""));
}
