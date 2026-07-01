static const wchar_t* WINDOW_TITLE  = L"Classic Launcher";
static const int      WINDOW_WIDTH  = 800;
static const int      WINDOW_HEIGHT = 600;

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
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
    const std::string manifest   = cache_dir + "\\LatestFileList.bin";

    LOG_STAT("CORE_PATCH", "Querying patch server...");
    try {
        PatchInfo info = patch_query();

        LOG_STAT("CORE_PATCH", "Server version : " + std::to_string(info.latest_version));
        LOG_STAT("CORE_PATCH", "List URL       : " + info.list_file_url);
        LOG_STAT("CORE_PATCH", "URL prefix     : " + info.url_prefix);

        auto all_files = patch_get_file_list(info, manifest);
        LOG_STAT("CORE_PATCH", "Manifest: " + std::to_string(all_files.size()) + " files");

        if (auto login_pf = patch_find_login_messages(all_files)) {
            patch_update_login_messages(info, *login_pf, login_messages_path(),
                                        cache_path);
        } else {
            LOG_WARN("CORE_PATCH", "LoginMessages.xml not found in manifest.");
        }

        PatchCache cache  = patch_cache_load(cache_path);
        auto       needed = patch_filter_needed(all_files, cache);
        LOG_STAT("CORE_PATCH", "Need update: " + std::to_string(needed.size()) + " files");

        if (needed.empty()) {
            PostMessage(g_hwnd, WM_PATCH_SHOW,  0, 0);
            PostMessage(g_hwnd, WM_PATCH_FILE,  100, reinterpret_cast<LPARAM>(new std::string()));
            PostMessage(g_hwnd, WM_PATCH_TOTAL, 100, 0);
            LOG_STAT("CORE_PATCH", "Already up to date.");
            return;
        }

        PostMessage(g_hwnd, WM_PATCH_SHOW, 0, 0);

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
// LoginMessages.xml validator
//
// Parses MSG_USER_AUTHEN_V3 out of %APPDATA%\wizlauncher\LoginMessages.xml and
// checks that every wire field (name + type, in order) matches what we
// hardcode in login_authenticate().  NOXFER metadata fields are skipped.
//
// If MSG_USER_AUTHEN_V4 is present, validation fails immediately : the server
// may have moved to a new auth message we do not implement yet.
//
// On the first run the file does not exist yet (downloaded during first patch);
// validation is skipped until the following launch.
//
// Returns an empty string on success or when the file is absent (first run).
// Returns an error description on mismatch or parse failure.
// ---------------------------------------------------------------------------

struct DmlFieldDef { std::string name; std::string type; };

static const DmlFieldDef k_authen_v3_expected[] = {
    { "Rec1",             "STR"  },
    { "Version",          "STR"  },
    { "Revision",         "STR"  },
    { "DataRevision",     "STR"  },
    { "CRC",              "STR"  },
    { "MachineID",        "GID"  },
    { "Locale",           "STR"  },
    { "PatchClientID",    "STR"  },
    { "IsSteamPatcher",   "UINT" },
    { "ConsoleType",      "UBYT" },
    { "PlatformChatID",   "STR"  },
    { "SteamID",          "STR"  },
    { "SteamAuthTicket",  "STR"  },
};

static std::string xml_attr(const std::string& tag, const std::string& key)
{
    size_t pos = tag.find(key + "=\"");
    char   q   = '"';
    if (pos == std::string::npos) {
        pos = tag.find(key + "='");
        q   = '\'';
    }
    if (pos == std::string::npos) return {};
    pos += key.size() + 2;
    size_t end = tag.find(q, pos);
    if (end == std::string::npos) return {};
    return tag.substr(pos, end - pos);
}

static std::string validate_login_messages_xml()
{
    const std::string path = login_messages_path();
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
    LOG_STAT("CORE_VALIDATE", "Validating: " + path);

    std::ifstream f(path);
    if (!f) return "Could not open " + path;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string xml = ss.str();

    if (xml.find("<MSG_USER_AUTHEN_V4>") != std::string::npos) {
        return "MSG_USER_AUTHEN_V4 found in " + path + ".\n\n"
               "wizlauncher does not support this protocol version yet.";
    }

    const std::string open_tag  = "<MSG_USER_AUTHEN_V3>";
    const std::string close_tag = "</MSG_USER_AUTHEN_V3>";
    size_t block_start = xml.find(open_tag);
    size_t block_end   = xml.find(close_tag);
    if (block_start == std::string::npos || block_end == std::string::npos)
        return "MSG_USER_AUTHEN_V3 block not found in " + path;

    std::string block = xml.substr(block_start + open_tag.size(),
                                   block_end - block_start - open_tag.size());

    std::vector<DmlFieldDef> found;
    size_t p = 0;
    while (p < block.size()) {
        size_t lt = block.find('<', p);
        if (lt == std::string::npos) break;
        size_t gt = block.find('>', lt);
        if (gt == std::string::npos) break;

        std::string tag = block.substr(lt + 1, gt - lt - 1);
        p = gt + 1;

        if (tag.empty() || tag[0] == '/' || tag == "RECORD") continue;
        if (tag.find("NOXFER") != std::string::npos) continue;

        size_t sp = tag.find_first_of(" \t\r\n/");
        std::string elem_name = (sp != std::string::npos) ? tag.substr(0, sp) : tag;
        std::string type_val  = xml_attr(tag, "TYPE");
        if (elem_name.empty() || type_val.empty()) continue;

        for (auto& c : type_val)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        found.push_back({ elem_name, type_val });
    }

    const size_t exp_count = sizeof(k_authen_v3_expected) / sizeof(k_authen_v3_expected[0]);
    if (found.size() != exp_count) {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "MSG_USER_AUTHEN_V3 field count mismatch:\n"
            "  expected %zu field(s), file has %zu.\n\n"
            "wizlauncher needs to be updated to match the new protocol.",
            exp_count, found.size());
        return buf;
    }
    for (size_t i = 0; i < exp_count; ++i) {
        if (found[i].name != k_authen_v3_expected[i].name ||
            found[i].type != k_authen_v3_expected[i].type)
        {
            char buf[512];
            snprintf(buf, sizeof(buf),
                "MSG_USER_AUTHEN_V3 field %zu mismatch:\n"
                "  expected  %-20s  %s\n"
                "  file has  %-20s  %s\n\n"
                "wizlauncher needs to be updated to match the new protocol.",
                i + 1,
                k_authen_v3_expected[i].name.c_str(),
                k_authen_v3_expected[i].type.c_str(),
                found[i].name.c_str(),
                found[i].type.c_str());
            return buf;
        }
    }
    LOG_STAT("CORE_VALIDATE", "MSG_USER_AUTHEN_V3 schema OK.");
    return {};
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

    if (!g_config.use_orig_auth) {
        std::string err = validate_login_messages_xml();
        if (!err.empty()) {
            LOG_ERR("CORE_VALIDATE", err);
            MessageBoxA(hwnd, err.c_str(), "Protocol Mismatch : wizlauncher Outdated",
                        MB_OK | MB_ICONERROR);
            return 1;
        }
    }

    LOG_STAT("CORE_CONFIG", "install_dir : " + g_config.install_dir);
    LOG_STAT("CORE_CONFIG", "uuid        : " + g_config.uuid);
    LOG_STAT("CORE_CONFIG", "hwid        : " + g_config.hwid);

    {
        uint64_t mid = get_machine_id_from_mac();
        char mid_hex[17];
        snprintf(mid_hex, sizeof(mid_hex), "%016llX",
                 static_cast<unsigned long long>(mid));
        std::string patch_client_id = g_config.uuid + ":{" + g_config.hwid + "}";
        LOG_STAT("CORE_CONFIG", "machine_id  : " + std::string(mid_hex));
        LOG_STAT("CORE_CONFIG", "PatchClientID: " + patch_client_id);
    }

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
        if (g_config.use_orig_auth)
            launch_orig_auth(g_config.install_dir,
                             g_patch_elapsed_secs,
                             g_hwnd,
                             g_pending_username,
                             g_pending_password,
                             g_config.game_resolution);
        else
            launch_authenticated(g_config.install_dir,
                                 g_patch_elapsed_secs,
                                 g_hwnd,
                                 g_login_result.ck2,
                                 g_login_result.user_id,
                                 g_login_result.username,
                                 g_config.game_resolution);
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
        MessageBoxA(hwnd, err->c_str(), "Login Failed", MB_OK | MB_ICONERROR);
        webview_execute(L"__showLoginError();");
        delete err;
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
