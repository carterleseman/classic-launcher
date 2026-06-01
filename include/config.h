#pragma once

#include <string>
#include <cstdint>

// ---------------------------------------------------------------------------
//  %APPDATA%\wizlauncher\config.json.
//
//  Fields are populated lazily: on first launch each missing field is
//  discovered from hardware/registry, then saved so future launches are
//  instant.
// ---------------------------------------------------------------------------
struct AppConfig {
    // Source: Uninstall\DisplayName="Wizard101" -> InstallLocation (HKCU first, then
    // HKLM\SOFTWARE\WOW6432Node\...\Uninstall).
    std::string install_dir;

    // KingsIsle per-user UUID : first part of PatchClientID.
    // Source: HKCU\Software\KingsIsle -> UUID
    std::string uuid;

    // HWID : second part of PatchClientID (no outer braces; login wraps with ':').
    //
    // Source: SMBIOS Type 2 (Baseboard) and Type 17 (Memory Device) strings,
    // filtered to alnum chars, sorted, joined with ':', then MD5-hashed.
    //
    // When SMBIOS yields no usable entries, value is the literal "HW-ID-SMBIOS"
    std::string hwid;

    // Lock the window to exactly 800x600 physical pixels regardless of DPI.
    // When false, Windows bitmap-scales the window to match the system DPI,
    // which looks correct on high-DPI displays but may appear blurry.
    bool fixed_window_size = true;

    bool quick_launch = false;

    // Page to show on startup : matches a page's data-title attribute.
    // Empty means the default (Ravenwood News).
    std::string starting_page;

    // Last username the user chose to remember. Empty when not set.
    std::string remembered_username;

    // When true, the password is also saved (DPAPI-encrypted) and autofilled.
    bool remember_password = false;

    // DPAPI-encrypted, base64-encoded password blob. Empty when not set.
    // Written by config_save; populated only when remember_password is true.
    std::string remembered_password;
};

// Returns the path to the config file: %APPDATA%\wizlauncher\config.json
std::string config_path();

// Login protocol schema
// Written during patching; absent on first launch.
// Path: %APPDATA%\wizlauncher\LoginMessages.xml
std::string login_messages_path();

AppConfig config_load();

void config_save(const AppConfig& cfg);
