static const wchar_t* WINDOW_TITLE  = L"Classic Launcher";
static const int      WINDOW_WIDTH  = 800;
static const int      WINDOW_HEIGHT = 600;

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <string>
#include <vector>
#include <thread>

#include "news.h"
#include "login_client.h"
#include "patch_client.h"
#include "config.h"
#include "accounts.h"
#include "json.h"
#include "crypto.h"
#include "logger.h"
#include "resource_ids.h"
#include "launcher.h"
#include "preferences.h"
#include "webview.h"

// ---------------------------------------------------------------------------

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

// ---------------------------------------------------------------------------
// Custom window messages : posted from background threads so that
// ExecuteScript is always called from the UI (COM) thread in WndProc.
//
//  WM_PATCH_SHOW   : switch to the patch-progress view
//                    WPARAM=0, LPARAM=0
//
//  WM_PATCH_FILE   : per-file byte progress
//                    WPARAM=percent (0-100)
//                    LPARAM=pointer to heap std::string (tar_name); WndProc frees it
//
//  WM_PATCH_TOTAL  : overall session progress
//                    WPARAM=percent (0-100), LPARAM=0
// ---------------------------------------------------------------------------
#define WM_PATCH_SHOW  (WM_APP + 1)
#define WM_PATCH_FILE  (WM_APP + 2)
#define WM_PATCH_TOTAL (WM_APP + 3)
// Login error, posted from the auth background thread.
//   WPARAM=0, LPARAM=pointer to heap std::string (message); WndProc frees it.
#define WM_LOGIN_ERROR (WM_APP + 4)
// WM_NEWS_READY is defined in news.h as (WM_APP + 5).
// Quick-launch: posted from the login thread when quick_launch skips patching.
#define WM_QUICK_LAUNCH (WM_APP + 6)

// ---------------------------------------------------------------------------

HWND       g_hwnd;
AppConfig  g_config;

// Elapsed patch time in whole seconds : set by run_patch(), read by launch functions.
DWORD g_patch_elapsed_secs = 0;

// Saved on successful custom auth; read when the user clicks PLAY.
LoginResult g_login_result{};

// Original auth mode only: credentials saved when the user clicks Login so
// they can be typed into the graphical interface.
std::string g_pending_username;
std::string g_pending_password;

// ---------------------------------------------------------------------------

static void run_patch()
{
    const std::string game_root  = g_config.install_dir;
    const std::string cache_path = patch_default_cache_path();
    const std::string cache_dir  = cache_path.substr(0, cache_path.find_last_of("/\\"));
    const std::string manifest   = game_root + "\\PatchInfo\\LatestFileList.bin";

    LOG_STAT("CORE_PATCH", "Querying patch server...");
    try {
        PatchInfo info = patch_query();

        LOG_STAT("CORE_PATCH", "Server version : " + std::to_string(info.latest_version));
        LOG_STAT("CORE_PATCH", "List URL       : " + info.list_file_url);
        LOG_STAT("CORE_PATCH", "URL prefix     : " + info.url_prefix);

        auto all_files = patch_get_file_list(info, manifest);
        LOG_STAT("CORE_PATCH", "Manifest: " + std::to_string(all_files.size()) + " files");

        PostMessage(g_hwnd, WM_PATCH_SHOW, 0, 0);
        PostMessage(g_hwnd, WM_PATCH_FILE, 0,
                    reinterpret_cast<LPARAM>(new std::string("Checking files")));

        PatchCache cache = patch_cache_load(cache_path);
        bool cache_changed = false;
        auto needed = patch_filter_needed(all_files, cache, game_root, cache_changed,
            [](int percent) {
                PostMessage(g_hwnd, WM_PATCH_FILE,
                            static_cast<WPARAM>(percent),
                            reinterpret_cast<LPARAM>(new std::string("Checking files")));
            });
        if (cache_changed)
            patch_cache_save(cache_path, cache);
        LOG_STAT("CORE_PATCH", "Need update: " + std::to_string(needed.size()) + " files");

        if (needed.empty()) {
            PostMessage(g_hwnd, WM_PATCH_FILE,  100, reinterpret_cast<LPARAM>(new std::string()));
            PostMessage(g_hwnd, WM_PATCH_TOTAL, 100, 0);
            LOG_STAT("CORE_PATCH", "Already up to date.");
            return;
        }


        ULONGLONG patch_start = GetTickCount64();

        PatchCallbacks cb;

        cb.on_file_progress = [](const std::string& name, int percent) {
            PostMessage(g_hwnd, WM_PATCH_FILE,
                        static_cast<WPARAM>(percent),
                        reinterpret_cast<LPARAM>(new std::string(name)));
        };

        cb.on_overall_progress = [](int percent) {
            PostMessage(g_hwnd, WM_PATCH_TOTAL, static_cast<WPARAM>(percent), 0);
        };

        patch_update(info, needed, game_root, cache_path, cb);
        g_patch_elapsed_secs = static_cast<DWORD>((GetTickCount64() - patch_start) / 1000);
        PostMessage(g_hwnd, WM_PATCH_TOTAL, 100, 0);
        LOG_STAT("CORE_PATCH", "Patch complete in " + std::to_string(g_patch_elapsed_secs) + "s.");
    }
    catch (const std::exception& ex) {
        std::string msg = std::string("Patch failed:\n\n") + ex.what();
        LOG_ERR("CORE_PATCH", msg);
        MessageBoxA(nullptr, msg.c_str(), "Patch : Failed", MB_OK | MB_ICONERROR);
    }
}

// ---------------------------------------------------------------------------

void run_login_and_patch(std::string username, std::string password)
{
    if (g_config.install_dir.empty()) {
        PostMessage(g_hwnd, WM_LOGIN_ERROR, 0,
                    reinterpret_cast<LPARAM>(new std::string(
                        "Game installation directory is not set.\n"
                        "Check that install_dir is present in config.json.")));
        return;
    }

    if (g_config.use_orig_auth) {
        g_pending_username = username;
        g_pending_password = password;
        LOG_STAT("CORE_LOGIN", "using original auth");

        if (g_config.quick_launch) {
            LOG_STAT("CORE_LOGIN", "quick launch: skipping patch, launching immediately");
            PostMessage(g_hwnd, WM_QUICK_LAUNCH, 0, 0);
        } else {
            run_patch();
        }
        return;
    }

    LOG_STAT("CORE_LOGIN", "Authenticating...");
    try {
        LoginResult result = login_authenticate(username, password,
                                               g_config.uuid, g_config.hwid);
        LOG_STAT("CORE_LOGIN", "Authenticated successfully.");

        g_login_result = result;

        if (g_config.quick_launch) {
            LOG_STAT("CORE_LOGIN", "Quick Launch : skipping patch, launching immediately.");
            PostMessage(g_hwnd, WM_QUICK_LAUNCH, 0, 0);
        } else {
            run_patch();
        }
    }
    catch (const std::exception& ex) {
        std::string msg = ex.what();
        LOG_ERR("CORE_LOGIN", "Failed: " + msg);
        PostMessage(g_hwnd, WM_LOGIN_ERROR, 0,
                    reinterpret_cast<LPARAM>(new std::string(msg)));
    }
}

// ---------------------------------------------------------------------------

static HWND create_window(HINSTANCE hInstance, int nCmdShow)
{
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm       = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"WizLauncherWindow";
    RegisterClassExW(&wc);

    // WS_POPUP : no title bar, no standard chrome, fixed size.
    // WS_EX_APPWINDOW : forces a taskbar button despite using WS_POPUP.
    int x = (GetSystemMetrics(SM_CXSCREEN) - WINDOW_WIDTH)  / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - WINDOW_HEIGHT) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"WizLauncherWindow",
        WINDOW_TITLE,
        WS_POPUP,
        x, y,
        WINDOW_WIDTH, WINDOW_HEIGHT,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hwnd) return nullptr;

    g_hwnd = hwnd;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    return hwnd;
}

static int run_message_loop()
{
    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return static_cast<int>(msg.wParam);
}

// ---------------------------------------------------------------------------

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int nCmdShow)
{
    log_init();
    extract_ui_resources();
    g_config = config_load();

    // Declare Per-Monitor DPI awareness before any window is created.
    if (g_config.fixed_window_size)
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HWND hwnd = create_window(hInstance, nCmdShow);
    if (!hwnd) return 1;

    init_webview(hwnd);

    return run_message_loop();
}

// ---------------------------------------------------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_PATCH_SHOW:
        webview_execute(L"__showPatchProgress();");
        return 0;

    case WM_PATCH_FILE: {
        int          percent = static_cast<int>(wParam);
        std::string* name    = reinterpret_cast<std::string*>(lParam);
        std::wstring wname(name->begin(), name->end());
        std::wstring js = L"__setFileProgress('" + wname
                        + L"', " + std::to_wstring(percent) + L");";
        webview_execute(js.c_str());
        delete name;
        return 0;
    }

    case WM_PATCH_TOTAL: {
        int percent = static_cast<int>(wParam);
        std::wstring js = L"__setOverallProgress(" + std::to_wstring(percent) + L");";
        webview_execute(js.c_str());
        return 0;
    }

    case WM_NEWS_READY:
        webview_on_news_ready();
        return 0;

    case WM_QUICK_LAUNCH:
        config_log(g_config);
        if (g_config.use_orig_auth)
            launch_orig_auth(g_config.install_dir,
                             g_patch_elapsed_secs,
                             g_hwnd,
                             g_pending_username,
                             g_pending_password,
                             g_config.game_resolution,
                             g_config.steam_tracking);
        else
            launch_authenticated(g_config.install_dir,
                                 g_patch_elapsed_secs,
                                 g_hwnd,
                                 g_login_result.ck2,
                                 g_login_result.user_id,
                                 g_login_result.username,
                                 g_config.game_resolution,
                                 g_config.steam_tracking);
        return 0;

    case WM_LAUNCH_DONE:
        if (g_config.keep_open) {
            ShowWindow(g_hwnd, SW_SHOW);
            SetForegroundWindow(g_hwnd);
            g_login_result    = {};
            g_pending_username.clear();
            g_pending_password.clear();
            g_patch_elapsed_secs = 0;
            webview_execute(L"__resetToLogin();");
        } else {
            PostMessage(g_hwnd, WM_CLOSE, 0, 0);
        }
        return 0;

    case WM_LOGIN_ERROR: {
        std::string* err = reinterpret_cast<std::string*>(lParam);
        const bool quarantined = err->find("AuthQuarantined") != std::string::npos;
        const bool banned      = err->find("AuthBanned") != std::string::npos;
        if (quarantined || banned) {
            std::wstring script = L"__showAccountNotification('"
                + std::wstring(quarantined ? L"AuthQuarantined" : L"AuthBanned")
                + L"');";
            webview_execute(script.c_str());
        } else {
            MessageBoxA(hwnd, err->c_str(), "Login Failed", MB_OK | MB_ICONERROR);
        }
        webview_execute(L"__showLoginError();");
        delete err;
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
