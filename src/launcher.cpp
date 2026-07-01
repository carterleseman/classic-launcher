#include "../include/launcher.h"
#include "../include/logger.h"
#include "../include/preferences.h"

#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <set>

// ---------------------------------------------------------------------------
// Shared spawn helper
// ---------------------------------------------------------------------------

// Builds the command line, spawns the process, and returns true on success.
static bool spawn_game(const std::string& exe,
                       const std::string& cmd,
                       const std::string& bin_dir,
                       HWND               owner)
{
    std::vector<char> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back('\0');

    STARTUPINFOA        si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessA(exe.c_str(), cmd_buf.data(),
                        nullptr, nullptr, FALSE, 0,
                        nullptr, bin_dir.c_str(), &si, &pi))
    {
        DWORD err = GetLastError();
        std::string msg = "CreateProcess failed (error " + std::to_string(err) + ").\n"
                        + "Executable: " + exe;
        LOG_ERR("LAUNCH", msg);
        MessageBoxA(owner, msg.c_str(), "Launch Failed", MB_OK | MB_ICONERROR);
        return false;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// ---------------------------------------------------------------------------

void launch_authenticated(const std::string& install_dir,
                          DWORD              patch_elapsed_secs,
                          HWND               launcher_hwnd,
                          const std::string& ck2,
                          uint64_t           user_id,
                          const std::string& username,
                          const std::string& game_resolution)
{
    apply_video_settings(install_dir, game_resolution);

    std::string exe     = install_dir + "\\Bin\\WizardGraphicalClient.exe";
    std::string bin_dir = install_dir + "\\Bin";

    std::string cmd = "\"" + exe + "\""
                    + " -L login.us.wizard101.com 12000"
                    + " -PT " + std::to_string(patch_elapsed_secs)
                    + " -U .." + std::to_string(user_id)
                    + " "      + ck2
                    + " "      + username;

    LOG_STAT("LAUNCH", "exe : " + exe);
    LOG_STAT("LAUNCH", "cmd : " + cmd);

    if (spawn_game(exe, cmd, bin_dir, launcher_hwnd))
        PostMessage(launcher_hwnd, WM_LAUNCH_DONE, 0, 0);
}

// ---------------------------------------------------------------------------
// Native auth window helpers
// ---------------------------------------------------------------------------

static std::set<HWND> get_wiz_windows()
{
    std::set<HWND> out;
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        char cls[64]{};
        GetClassNameA(hwnd, cls, sizeof(cls));
        if (std::string(cls) == "Wizard Graphical Client")
            reinterpret_cast<std::set<HWND>*>(lp)->insert(hwnd);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&out));
    return out;
}

static std::string get_window_title(HWND hwnd)
{
    char buf[256]{};
    GetWindowTextA(hwnd, buf, sizeof(buf));
    return buf;
}

// ---------------------------------------------------------------------------
// Native auth automation thread
// ---------------------------------------------------------------------------

static void automate_login(const std::string& username,
                           const std::string& password,
                           std::set<HWND>     existing,
                           HWND               launcher)
{
    // Wait up to 60 s for the new "Wizard Graphical Client" window to appear.
    HWND game = nullptr;
    for (int i = 0; i < 600 && !game; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        for (HWND h : get_wiz_windows()) {
            if (existing.find(h) == existing.end()) {
                game = h;
                break;
            }
        }
    }

    if (!game) {
        LOG_ERR("LAUNCH", "timed out waiting for Wizard Graphical Client window.");
        PostMessage(launcher, WM_CLOSE, 0, 0);
        return;
    }

    LOG_STAT("LAUNCH", "game window found: waiting for login ui...");

    // Wait up to 60 s for the window title to become "Wizard101".
    for (int i = 0; i < 600; ++i) {
        if (get_window_title(game) == "Wizard101") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (get_window_title(game) != "Wizard101") {
        LOG_ERR("LAUNCH", "timed out waiting for login ui");
        PostMessage(launcher, WM_CLOSE, 0, 0);
        return;
    }

    LOG_STAT("LAUNCH", "login ui ready");

    // Brief pause to let the UI settle before accepting input.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // Username field is focused by default.
    for (unsigned char c : username)
        SendMessageA(game, WM_CHAR, c, 0);

    // Tab to the password field.
    SendMessageA(game, WM_CHAR, VK_TAB, 0);

    for (unsigned char c : password)
        SendMessageA(game, WM_CHAR, c, 0);

    SendMessageA(game, WM_CHAR, VK_RETURN, 0);

    PostMessage(launcher, WM_LAUNCH_DONE, 0, 0);
}

// ---------------------------------------------------------------------------

void launch_orig_auth(const std::string& install_dir,
                      DWORD              patch_elapsed_secs,
                      HWND               launcher_hwnd,
                      const std::string& username,
                      const std::string& password,
                      const std::string& game_resolution)
{
    apply_video_settings(install_dir, game_resolution);

    std::string exe     = install_dir + "\\Bin\\WizardGraphicalClient.exe";
    std::string bin_dir = install_dir + "\\Bin";

    std::string cmd = "\"" + exe + "\""
                    + " -L login.us.wizard101.com 12000"
                    + " -PT " + std::to_string(patch_elapsed_secs);

    LOG_STAT("LAUNCH", "exe : " + exe);
    LOG_STAT("LAUNCH", "cmd : " + cmd);

    // Snapshot existing game windows before spawning.
    auto existing = get_wiz_windows();

    if (!spawn_game(exe, cmd, bin_dir, launcher_hwnd))
        return;

    // Hide the launcher so it stays alive during automation
    ShowWindow(launcher_hwnd, SW_HIDE);

    // Automation thread posts WM_CLOSE once credentials have been sent.
    std::thread([username, password, existing, launcher_hwnd]() {
        automate_login(username, password, existing, launcher_hwnd);
    }).detach();
}
