#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

// Custom authentication launch: -L -PT -U <user_id> <ck2> <username>
void launch_authenticated(const std::string& install_dir,
                          DWORD              patch_elapsed_secs,
                          HWND               launcher_hwnd,
                          const std::string& ck2,
                          uint64_t           user_id,
                          const std::string& username);

// Native auth launch: -L -PT (no -U).
// Hides the launcher window, then automates the in-game login UI by
// sending WM_CHAR messages once the game window is ready.
void launch_orig_auth(const std::string& install_dir,
                      DWORD              patch_elapsed_secs,
                      HWND               launcher_hwnd,
                      const std::string& username,
                      const std::string& password);
