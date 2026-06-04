static const wchar_t* WINDOW_TITLE  = L"Classic Launcher";
static const int      WINDOW_WIDTH  = 800;
static const int      WINDOW_HEIGHT = 600;

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <WebView2.h>
#include <string>
#include <vector>
#include <functional>
#include <fstream>
#include <sstream>
#include <memory>

#include "news.h"
#include "login_client.h"
#include "patch_client.h"
#include "config.h"
#include "crypto.h"
#include "logger.h"
#include "resource_ids.h"
#include "launcher.h"

#include <thread>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;


LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static void      resize_webview(HWND hwnd);
static void      create_main_controller(ICoreWebView2Environment* env, HWND hwnd);

// ---------------------------------------------------------------------------
// Custom window messages : posted from the patch background thread so that
// ExecuteScript is always called from the UI (COM) thread in WndProc.
//
//  WM_PATCH_SHOW   : tell the UI to switch to the patch-progress view
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


static HWND                            g_hwnd;
static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2>           g_webview;
static EventRegistrationToken          g_webmsg_token{};
static AppConfig                       g_config;

// Base path for extracted UI files: %APPDATA%\wizlauncher\ (trailing backslash).
// Set once by extract_ui_resources() before WebView2 is initialised.
static std::wstring g_ui_base;

// Elapsed patch time in whole seconds : set by run_patch(), read by launch_game().
static DWORD g_patch_elapsed_secs = 0;

// Synchronisation for the two independent async paths:
//   news_start_fetch (background WinHTTP) and create_main_controller (WebView2).
// Both run in parallel; whichever finishes last calls news_make_script().
static bool g_nav_complete = false; // true after NavigationCompleted fires for ui.html
static bool g_news_ready   = false; // true after WM_NEWS_READY is handled

// Saved on successful login; read by launch_game when the user clicks PLAY.
// Written by the login background thread before patching begins
static LoginResult  g_login_result{};

// original auth mode only: credentials saved when the user clicks Login so
// they can be typed into the graphical interface
static std::string  g_pending_username;
static std::string  g_pending_password;


// ---------------------------------------------------------------------------
// Narrow-string helper : converts ASCII-range wstring to string.
// ---------------------------------------------------------------------------
static std::string wstring_to_narrow(const std::wstring& ws)
{
    std::string out;
    out.reserve(ws.size());
    for (wchar_t wc : ws) out += static_cast<char>(wc);
    return out;
}

// ---------------------------------------------------------------------------
// Extracts and JSON-unescapes a string value by key from a flat JSON object.
// Handles \\ \/ \" \n \r \t : sufficient for all fields including file paths.
// ---------------------------------------------------------------------------
static std::wstring json_get_wstring(const std::wstring& json, const std::wstring& key)
{
    std::wstring needle = L"\"" + key + L"\":\"";
    auto pos = json.find(needle);
    if (pos == std::wstring::npos) return {};
    pos += needle.size();

    std::wstring out;
    for (size_t i = pos; i < json.size(); ++i) {
        if (json[i] == L'\\' && i + 1 < json.size()) {
            switch (json[++i]) {
                case L'\\': out += L'\\'; break;
                case L'/':  out += L'/';  break;
                case L'"':  out += L'"';  break;
                case L'n':  out += L'\n'; break;
                case L'r':  out += L'\r'; break;
                case L't':  out += L'\t'; break;
                default:    out += json[i]; break;
            }
        } else if (json[i] == L'"') {
            break;
        } else {
            out += json[i];
        }
    }
    return out;
}

static bool json_get_wbool(const std::wstring& json, const std::wstring& key)
{
    std::wstring needle = L"\"" + key + L"\":";
    auto pos = json.find(needle);
    if (pos == std::wstring::npos) return false;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == L' ' || json[pos] == L'\t')) ++pos;
    return pos + 4 <= json.size() && json.substr(pos, 4) == L"true";
}

// ---------------------------------------------------------------------------
// Queries the patch server, filters against the local cache, then drives
// patch_update with progress callbacks that route to the WebView2 UI via
// PostMessage -> WndProc -> ExecuteScript.
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
            // Nothing to do : jump straight to 100% and let the UI know.
            PostMessage(g_hwnd, WM_PATCH_SHOW,  0, 0);
            PostMessage(g_hwnd, WM_PATCH_FILE,  100, reinterpret_cast<LPARAM>(new std::string()));
            PostMessage(g_hwnd, WM_PATCH_TOTAL, 100, 0);
            LOG_STAT("CORE_PATCH", "Already up to date.");
            return;
        }

        // Tell the UI to switch to the patch-progress view.
        PostMessage(g_hwnd, WM_PATCH_SHOW, 0, 0);

        ULONGLONG patch_start = GetTickCount64();

        PatchCallbacks cb;

        cb.on_file_progress = [](const std::string& name, int percent) {
            // Heap-allocate the name so it survives the PostMessage round-trip.
            PostMessage(g_hwnd, WM_PATCH_FILE,
                        static_cast<WPARAM>(percent),
                        reinterpret_cast<LPARAM>(new std::string(name)));
        };

        cb.on_overall_progress = [](int percent) {
            PostMessage(g_hwnd, WM_PATCH_TOTAL, static_cast<WPARAM>(percent), 0);
        };

        patch_update(info, needed, game_root, cache_path, cb);
        g_patch_elapsed_secs = static_cast<DWORD>((GetTickCount64() - patch_start) / 1000);
        // Guarantee 100% is always posted after a successful run regardless of
        // whether patch_update's last callback rounded correctly.
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
// Validates credentials first; on success saves the result and starts patching.
// On failure posts WM_LOGIN_ERROR so the UI can show the reason and re-enable
// the Login button.
// ---------------------------------------------------------------------------
static void run_login_and_patch(std::string username, std::string password)
{
    if (g_config.install_dir.empty()) {
        PostMessage(g_hwnd, WM_LOGIN_ERROR, 0,
                    reinterpret_cast<LPARAM>(new std::string(
                        "Game installation directory is not set.\n"
                        "Check that install_dir is present in config.json.")));
        return;
    }

    if (g_config.use_orig_auth) {
        // Skip custom authentication : store credentials to
        // type into the graphical client's login UI after it loads.
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

        // Save before patching so launch_game can use it once PLAY is enabled.
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
// Step functions : called in order from wWinMain
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
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); // WebView covers this anyway
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

static void create_main_controller(ICoreWebView2Environment* env, HWND hwnd)
{
    // Build a file URI from g_ui_base (backslashes → forward slashes).
    std::wstring base_fwd = g_ui_base;
    for (auto& c : base_fwd) if (c == L'\\') c = L'/';
    std::wstring uri = L"file:///" + base_fwd + L"ui.html";

    env->CreateCoreWebView2Controller(
        hwnd,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [hwnd, uri](HRESULT hr, ICoreWebView2Controller* controller) -> HRESULT
            {
                if (FAILED(hr) || !controller) return hr;

                g_controller = controller;
                g_controller->get_CoreWebView2(&g_webview);

                // Fix DPI scaling: WebView2 auto-scales CSS pixels to match the
                // monitor DPI, shrinking the effective viewport on high-DPI displays.
                // Lock RasterizationScale to 1.0 so 1 CSS pixel always equals 1
                // physical pixel : the layout was designed for 800x600 physical pixels.
                if (g_config.fixed_window_size) {
                    ComPtr<ICoreWebView2Controller3> ctrl3;
                    if (SUCCEEDED(g_controller.As(&ctrl3))) {
                        ctrl3->put_ShouldDetectMonitorScaleChanges(FALSE);
                        ctrl3->put_RasterizationScale(1.0);
                    }
                }

                // Disable unwanted browser UI chrome
                ComPtr<ICoreWebView2Settings> settings;
                g_webview->get_Settings(&settings);
                settings->put_AreDefaultContextMenusEnabled(FALSE);
                settings->put_IsStatusBarEnabled(FALSE);
                settings->put_AreDevToolsEnabled(FALSE);

                // One-shot handler: fires when ui.html finishes loading.
                auto token = std::make_shared<EventRegistrationToken>();
                g_webview->add_NavigationCompleted(
                    Callback<ICoreWebView2NavigationCompletedEventHandler>(
                        [token](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs*) mutable -> HRESULT
                        {
                            sender->remove_NavigationCompleted(*token);

                            // Call the function directly : setting a global here would be
                            // too late since the page's inline scripts already ran.
                            std::wstring remembered(g_config.remembered_username.begin(),
                                                    g_config.remembered_username.end());
                            std::wstring init_script =
                                L"__setRememberedUsername('" + remembered + L"');";
                            sender->ExecuteScript(init_script.c_str(), nullptr);

                            // Autofill the password if remember_password is on and a
                            // blob exists. Silently skip if decryption fails (wrong machine).
                            if (g_config.remember_password && !g_config.remembered_password.empty()) {
                                std::string pw = crypto_dpapi_decrypt(g_config.remembered_password);
                                if (!pw.empty()) {
                                    // Escape single quotes in the password for the JS literal.
                                    std::wstring wpw;
                                    for (unsigned char c : pw) {
                                        if (c == '\'') wpw += L"\\'";
                                        else if (c == '\\') wpw += L"\\\\";
                                        else wpw += static_cast<wchar_t>(c);
                                    }
                                    std::wstring pw_script = L"__setRememberedPassword('" + wpw + L"');";
                                    OutputDebugStringA(("[DBG] remembered password: " + pw + "\n").c_str());
                                    sender->ExecuteScript(pw_script.c_str(), nullptr);
                                } else {
                                    // Blob is unreadable on this machine : clear it.
                                    g_config.remembered_password.clear();
                                    config_save(g_config);
                                }
                            }

                            // Populate the settings page with stored values.
                            // Escape backslashes in install_dir for a JS string literal.
                            auto js_str_escape = [](const std::string& s) {
                                std::wstring out;
                                for (unsigned char c : s) {
                                    if (c == '\\') out += L"\\\\";
                                    else if (c == '\'') out += L"\\'";
                                    else out += static_cast<wchar_t>(c);
                                }
                                return out;
                            };
                            std::wstring w_starting_page(g_config.starting_page.begin(),
                                                         g_config.starting_page.end());
                            std::wstring settings_script =
                                L"__initSettings('"
                                + js_str_escape(g_config.install_dir) + L"',"
                                + (g_config.fixed_window_size  ? L"true" : L"false") + L","
                                + (g_config.quick_launch       ? L"true" : L"false") + L","
                                + (g_config.use_orig_auth      ? L"true" : L"false") + L","
                                + (g_config.remember_password  ? L"true" : L"false") + L","
                                + L"'" + w_starting_page + L"');";
                            sender->ExecuteScript(settings_script.c_str(), nullptr);

                            // If the WinHTTP news fetch already completed, render now;
                            // otherwise WM_NEWS_READY will call news_make_script() later.
                            g_nav_complete = true;
                            LOG_STAT("CORE_NEWS", "NavigationCompleted : ui ready");
                            if (g_news_ready) {
                                LOG_STAT("CORE_NEWS", "news already fetched : running extraction script");
                                sender->ExecuteScript(news_make_script().c_str(), nullptr);
                            } else {
                                LOG_STAT("CORE_NEWS", "waiting for WM_NEWS_READY");
                            }

                            return S_OK;
                        }
                    ).Get(),
                    token.get()
                );

                // Handle messages posted from JS via window.chrome.webview.postMessage().
                // Expected formats:
                //   {"action":"login","username":"...","password":"..."}
                //   {"action":"play"}
                g_webview->add_WebMessageReceived(
                    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT
                        {
                            LPWSTR raw = nullptr;
                            if (FAILED(args->TryGetWebMessageAsString(&raw)) || !raw)
                                return S_OK;

                            std::wstring msg(raw);
                            CoTaskMemFree(raw);

                            std::wstring action = json_get_wstring(msg, L"action");

                            if (action == L"login") {
                                std::string u = wstring_to_narrow(json_get_wstring(msg, L"username"));
                                std::string p = wstring_to_narrow(json_get_wstring(msg, L"password"));

                                bool remember = json_get_wbool(msg, L"remember");
                                g_config.remembered_username = remember ? u : "";
                                if (g_config.remember_password)
                                    g_config.remembered_password = crypto_dpapi_encrypt(p);
                                config_save(g_config);

                                std::thread([u, p]() { run_login_and_patch(u, p); }).detach();
                            }
                            else if (action == L"save_settings") {
                                g_config.install_dir        = wstring_to_narrow(json_get_wstring(msg, L"install_dir"));
                                g_config.fixed_window_size  = json_get_wbool(msg, L"fixed_window_size");
                                g_config.quick_launch       = json_get_wbool(msg, L"quick_launch");
                                g_config.use_orig_auth      = json_get_wbool(msg, L"use_orig_auth");
                                g_config.remember_password  = json_get_wbool(msg, L"remember_password");
                                g_config.starting_page      = wstring_to_narrow(json_get_wstring(msg, L"starting_page"));
                                // Clear the stored blob when the feature is turned off.
                                if (!g_config.remember_password)
                                    g_config.remembered_password.clear();
                                config_save(g_config);
                                LOG_STAT("CORE_CONFIG", "Settings saved.");
                            }
                            else if (action == L"play") {
                                if (g_config.use_orig_auth)
                                    launch_orig_auth(g_config.install_dir,
                                                     g_patch_elapsed_secs,
                                                     g_hwnd,
                                                     g_pending_username,
                                                     g_pending_password);
                                else
                                    launch_authenticated(g_config.install_dir,
                                                         g_patch_elapsed_secs,
                                                         g_hwnd,
                                                         g_login_result.ck2,
                                                         g_login_result.user_id,
                                                         g_login_result.username);
                            }
                            else if (action == L"drag") {
                                ReleaseCapture();
                                SendMessage(g_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                            }
                            else if (action == L"minimize") {
                                ShowWindow(g_hwnd, SW_MINIMIZE);
                            }
                            else if (action == L"close") {
                                PostMessage(g_hwnd, WM_CLOSE, 0, 0);
                            }

                            return S_OK;
                        }
                    ).Get(),
                    &g_webmsg_token
                );

                // Open all http/https links in the system default browser.
                // target="_blank" links fire NewWindowRequested; same-frame
                // navigations (if any) are caught by NavigationStarting.
                g_webview->add_NewWindowRequested(
                    Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT
                        {
                            LPWSTR uri_raw = nullptr;
                            if (SUCCEEDED(args->get_Uri(&uri_raw)) && uri_raw) {
                                std::wstring url(uri_raw);
                                CoTaskMemFree(uri_raw);
                                if (url.rfind(L"http", 0) == 0)
                                    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                            }
                            args->put_Handled(TRUE);
                            return S_OK;
                        }
                    ).Get(),
                    nullptr
                );

                g_webview->add_NavigationStarting(
                    Callback<ICoreWebView2NavigationStartingEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT
                        {
                            LPWSTR uri_raw = nullptr;
                            if (SUCCEEDED(args->get_Uri(&uri_raw)) && uri_raw) {
                                std::wstring url(uri_raw);
                                CoTaskMemFree(uri_raw);
                                if (url.rfind(L"http", 0) == 0) {
                                    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                                    args->put_Cancel(TRUE);
                                }
                            }
                            return S_OK;
                        }
                    ).Get(),
                    nullptr
                );

                resize_webview(hwnd);
                g_webview->Navigate(uri.c_str());

                return S_OK;
            }
        ).Get()
    );
}

// ---------------------------------------------------------------------------
// Embedded UI files : mapping from resource ID to relative output path.
// ---------------------------------------------------------------------------
struct EmbeddedFile { int id; const char* rel_path; };
static const EmbeddedFile k_ui_files[] = {
    { IDR_UI_HTML,               "ui.html"                          },
    { IDR_UI_CSS,                "ui.css"                           },
    { IDR_ASSET_BANNER,          "assets\\banner.webp"              },
    { IDR_ASSET_BUG,             "assets\\bug.png"                  },
    { IDR_ASSET_BUTTON_BG,       "assets\\button-background.png"    },
    { IDR_ASSET_BUTTON_CLOSE,    "assets\\button-close.png"         },
    { IDR_ASSET_BUTTON_DOWN,     "assets\\button-down.png"          },
    { IDR_ASSET_BUTTON_MINIMIZE, "assets\\button-minimize.png"      },
    { IDR_ASSET_BUTTON_UP,       "assets\\button-up.png"            },
    { IDR_ASSET_BUTTON,          "assets\\button.png"               },
    { IDR_ASSET_CHECKBOX_CHECKED,"assets\\checkbox-checked.png"     },
    { IDR_ASSET_CHECKBOX,        "assets\\checkbox.png"             },
    { IDR_ASSET_PAGE_CHECKBOX,   "assets\\page-checkbox.png"        },
    { IDR_ASSET_FILE_PROG_BORDER,"assets\\file-progress-border.png" },
    { IDR_ASSET_FOOTER_BG,       "assets\\footer-background.webp"   },
    { IDR_ASSET_INPUT_BG,        "assets\\input-background.png"     },
    { IDR_ASSET_SECTION_BG,      "assets\\section-background.webp"  },
    { IDR_ASSET_SUPPORT,         "assets\\support.png"              },
    { IDR_ASSET_TAB_BUTTON,      "assets\\tab-button.png"           },
    { IDR_ASSET_TOTAL_PROG_BAR,  "assets\\total-progress-bar.png"   },
    { IDR_ASSET_W101_LOGO,       "assets\\w101-logo.webp"           },
    { IDR_ASSET_WINDOW_BORDER,   "assets\\window-border.webp"       },
    { IDR_ASSET_WIZ_16X16,       "assets\\wiz-16x16.png"            },
    { IDR_ASSET_HELP,            "assets\\help.png"                 },
    { IDR_BG_1,                  "assets\\backgrounds\\1.webp"      },
    { IDR_BG_2,                  "assets\\backgrounds\\2.webp"      },
    { IDR_BG_3,                  "assets\\backgrounds\\3.webp"      },
    { IDR_BG_4,                  "assets\\backgrounds\\4.webp"      },
    { IDR_BG_5,                  "assets\\backgrounds\\5.webp"      },
    { IDR_BG_6,                  "assets\\backgrounds\\6.webp"      },
    { IDR_BG_7,                  "assets\\backgrounds\\7.webp"      },
    { IDR_BG_8,                  "assets\\backgrounds\\8.webp"      },
    { IDR_BG_9,                  "assets\\backgrounds\\9.webp"      },
    { IDR_BG_10,                 "assets\\backgrounds\\10.webp"     },
    { IDR_BG_11,                 "assets\\backgrounds\\11.webp"     },
    { IDR_BG_12,                 "assets\\backgrounds\\12.webp"     },
    { IDR_BG_13,                 "assets\\backgrounds\\13.webp"     },
    { IDR_BG_14,                 "assets\\backgrounds\\14.webp"     },
    { IDR_BG_15,                 "assets\\backgrounds\\15.webp"     },
    { IDR_FONT_WIZARD_FANCY,     "assets\\fonts\\wizard_fancy.woff" },
};

static void ensure_parent_dirs(const std::wstring& path)
{
    for (size_t i = 0; i < path.size(); ++i)
        if (path[i] == L'\\')
            CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
}

// Extracts all embedded UI files to %APPDATA%\wizlauncher\ and sets g_ui_base.
// Runs synchronously before WebView2 is started : files are small and fast.
static void extract_ui_resources()
{
    wchar_t appdata[MAX_PATH]{};
    GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    g_ui_base = std::wstring(appdata) + L"\\wizlauncher\\";

    for (const auto& f : k_ui_files) {
        std::wstring rel;
        for (char c : std::string(f.rel_path)) rel += static_cast<wchar_t>(c);

        std::wstring out = g_ui_base + rel;
        ensure_parent_dirs(out);

        HRSRC   hrsrc = FindResourceW(nullptr, MAKEINTRESOURCEW(f.id), RT_RCDATA);
        if (!hrsrc) continue;
        HGLOBAL hg    = LoadResource(nullptr, hrsrc);
        if (!hg)    continue;
        const void* data = LockResource(hg);
        DWORD       size = SizeofResource(nullptr, hrsrc);

        HANDLE h = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(h, data, size, &written, nullptr);
            CloseHandle(h);
        }
    }

    {
        std::string narrow;
        for (wchar_t wc : g_ui_base) narrow += static_cast<char>(wc);
        LOG_STAT("CORE_INIT", "UI resources extracted to " + narrow);
    }
}

// Starts the async WebView2 initialization.
//   1. Create the environment (browser process + profile).
//   2. Kick off the WinHTTP news fetch on a background thread (WM_NEWS_READY).
//   3. Create the visible main controller immediately : no waiting for news.
static void init_webview(HWND hwnd)
{
    // Strip the trailing backslash : WebView2 requires no trailing separator.
    std::wstring user_data = g_ui_base;
    if (!user_data.empty() && user_data.back() == L'\\')
        user_data.pop_back();

    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, user_data.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT
            {
                if (FAILED(hr) || !env) return hr;

                // Start WinHTTP news fetch in the background; result arrives
                // via WM_NEWS_READY once the HTML has been fetched and encoded.
                news_start_fetch(hwnd);

                // Create the main controller right away : no sequential dependency
                // on the news fetch. The two async paths race; whichever finishes
                // last calls news_make_script() and renders the news articles.
                create_main_controller(env, hwnd);

                return S_OK;
            }
        ).Get()
    );
}

// Runs the Win32 message loop until the window is closed.
static int run_message_loop()
{
    // GetMessage blocks until a message arrives.
    // The loop exits when WM_QUIT is posted (via PostQuitMessage in WndProc).
    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg); // convert key-down events into WM_CHAR
        DispatchMessage(&msg);  // route the message to WndProc
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
// On the first run the file does not exist yet (it is downloaded during the
// first patch); validation is skipped until the following launch.
//
// Returns an empty string on success or when the file is absent (first run).
// Returns an error description on mismatch or parse failure.
// ---------------------------------------------------------------------------

struct DmlFieldDef { std::string name; std::string type; };

// The fields we send in MSG_USER_AUTHEN_V3, in wire order.
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

// Returns the value of attribute  key="..."  within a tag string.
// Returns empty string if absent.
static std::string xml_attr(const std::string& tag, const std::string& key)
{
    // Look for   key="value"  or  key='value'
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
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LOG_STAT("CORE_VALIDATE", "First run : LoginMessages.xml not yet in AppData; skipping.");
        return {};
    }
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

    // Walk every self-closing or open tag inside the block, collect wire fields.
    // A wire field is any tag that does NOT have NOXFER="TRUE".
    std::vector<DmlFieldDef> found;
    size_t p = 0;
    while (p < block.size()) {
        size_t lt = block.find('<', p);
        if (lt == std::string::npos) break;
        size_t gt = block.find('>', lt);
        if (gt == std::string::npos) break;

        std::string tag = block.substr(lt + 1, gt - lt - 1);
        p = gt + 1;

        // Skip closing tags and RECORD wrappers.
        if (tag.empty() || tag[0] == '/' || tag == "RECORD") continue;

        // Skip NOXFER metadata fields.
        if (tag.find("NOXFER") != std::string::npos) continue;

        // Extract element name (first token) and TYPE attribute.
        size_t sp = tag.find_first_of(" \t\r\n/");
        std::string elem_name = (sp != std::string::npos) ? tag.substr(0, sp) : tag;
        std::string type_val  = xml_attr(tag, "TYPE");
        if (elem_name.empty() || type_val.empty()) continue;

        // Uppercase the type for a case-insensitive comparison.
        for (auto& c : type_val)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        found.push_back({ elem_name, type_val });
    }

    // Compare against our hardcoded expectation.
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
// Entry point
// ---------------------------------------------------------------------------
int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int nCmdShow)
{
    // Load config first : needed before the window is created so fixed_window_size
    // can gate the DPI awareness call below.
    log_init();
    extract_ui_resources();
    g_config = config_load();

    // Declare Per-Monitor DPI awareness before any window is created.
    // Without this, Windows bitmap-scales the whole window at non-100% DPI,
    // making an 800x600 window appear as ~1200x900 at 150% scale.
    // WebView2 is fully DPI-aware and handles its own rendering correctly once
    // the host process opts in here.
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

    init_webview(hwnd); // fetches news then creates the main controller

    return run_message_loop();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Resizes the WebView to fill the entire client area of the window.
static void resize_webview(HWND hwnd)
{
    if (!g_controller) return;
    RECT bounds{};
    GetClientRect(hwnd, &bounds);
    g_controller->put_Bounds(bounds);
}

// Handles Win32 window messages.
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    // Switch the footer from the login view to the patch-progress view.
    case WM_PATCH_SHOW:
        if (g_webview)
            g_webview->ExecuteScript(L"__showPatchProgress();", nullptr);
        return 0;

    // Per-file byte progress: WPARAM=percent, LPARAM=heap std::string* (name).
    case WM_PATCH_FILE: {
        int          percent = static_cast<int>(wParam);
        std::string* name    = reinterpret_cast<std::string*>(lParam);
        if (g_webview) {
            // Build the JS call. Names are forward-slash paths with no special
            // characters that require escaping.
            std::wstring wname(name->begin(), name->end());
            std::wstring js = L"__setFileProgress('" + wname
                            + L"', " + std::to_wstring(percent) + L");";
            g_webview->ExecuteScript(js.c_str(), nullptr);
        }
        delete name;
        return 0;
    }

    // Overall session progress: WPARAM=percent.
    case WM_PATCH_TOTAL: {
        int percent = static_cast<int>(wParam);
        if (g_webview) {
            std::wstring js = L"__setOverallProgress(" + std::to_wstring(percent) + L");";
            g_webview->ExecuteScript(js.c_str(), nullptr);
        }
        return 0;
    }

    // News fetch complete: if ui.html has also loaded, extract and render now.
    case WM_NEWS_READY:
        LOG_STAT("CORE_NEWS", "WM_NEWS_READY received");
        g_news_ready = true;
        if (g_nav_complete && g_webview) {
            LOG_STAT("CORE_NEWS", "ui already loaded : running extraction script");
            std::wstring script = news_make_script();
            g_webview->ExecuteScript(script.c_str(), nullptr);
        } else {
            LOG_STAT("CORE_NEWS", "ui not yet loaded : will render in NavigationCompleted");
        }
        return 0;

    // Quick-launch: login succeeded with patching skipped; fire the game now.
    case WM_QUICK_LAUNCH:
        if (g_config.use_orig_auth)
            launch_orig_auth(g_config.install_dir,
                             g_patch_elapsed_secs,
                             g_hwnd,
                             g_pending_username,
                             g_pending_password);
        else
            launch_authenticated(g_config.install_dir,
                                 g_patch_elapsed_secs,
                                 g_hwnd,
                                 g_login_result.ck2,
                                 g_login_result.user_id,
                                 g_login_result.username);
        return 0;

    // Login failure: show a message box and re-enable the login button.
    case WM_LOGIN_ERROR: {
        std::string* err = reinterpret_cast<std::string*>(lParam);
        MessageBoxA(hwnd, err->c_str(), "Login Failed", MB_OK | MB_ICONERROR);
        if (g_webview)
            g_webview->ExecuteScript(L"__showLoginError();", nullptr);
        delete err;
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
