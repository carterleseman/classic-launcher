#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

// Both functions spawn WizardGraphicalClient.exe from install_dir\Bin\.
// On success each posts WM_LAUNCH_DONE to launcher_hwnd so the UI thread
// can decide whether to close the launcher or reset it to the login state

#define WM_LAUNCH_DONE (WM_APP + 7)

// Custom authentication launch: -L -PT -U <user_id> <ck2> <username>
void launch_authenticated(const std::string& install_dir,
                          DWORD              patch_elapsed_secs,
                          HWND               launcher_hwnd,
                          const std::string& ck2,
                          uint64_t           user_id,
                          const std::string& username,
                          const std::string& game_resolution);

// Native auth launch: -L -PT (no -U).
// Hides the launcher window, then automates the in-game login UI via WM_CHAR
void launch_orig_auth(const std::string& install_dir,
                      DWORD              patch_elapsed_secs,
                      HWND               launcher_hwnd,
                      const std::string& username,
                      const std::string& password,
                      const std::string& game_resolution);
