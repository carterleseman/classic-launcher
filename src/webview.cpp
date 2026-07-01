#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <WebView2.h>
#include <string>
#include <vector>
#include <memory>
#include <thread>

#include "webview.h"
#include "news.h"
#include "config.h"
#include "accounts.h"
#include "json.h"
#include "crypto.h"
#include "logger.h"
#include "resource_ids.h"
#include "launcher.h"
#include "login_client.h"
#include "preferences.h"

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;

// ---------------------------------------------------------------------------
// Externs : shared state owned by main.cpp
// ---------------------------------------------------------------------------
extern HWND        g_hwnd;
extern AppConfig   g_config;
extern LoginResult g_login_result;
extern std::string g_pending_username;
extern std::string g_pending_password;
extern DWORD       g_patch_elapsed_secs;

void run_login_and_patch(std::string username, std::string password);

// ---------------------------------------------------------------------------
// Locals
// ---------------------------------------------------------------------------
static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2>           g_webview;
static EventRegistrationToken          g_webmsg_token{};

// Base path for extracted UI files: %APPDATA%\wizlauncher\ (trailing backslash).
static std::wstring g_ui_base;

// Synchronisation for the two async paths (news fetch + WebView2 navigation).
static bool g_nav_complete = false;
static bool g_news_ready   = false;

// ---------------------------------------------------------------------------
// Internal string helpers
// ---------------------------------------------------------------------------
static std::string wstring_to_narrow(const std::wstring& ws)
{
    std::string out;
    out.reserve(ws.size());
    for (wchar_t wc : ws) out += static_cast<char>(wc);
    return out;
}

static std::wstring js_str_escape_w(const std::string& s)
{
    std::wstring out;
    for (unsigned char c : s) {
        if (c == '\\') out += L"\\\\";
        else if (c == '\'') out += L"\\'";
        else out += static_cast<wchar_t>(c);
    }
    return out;
}

static void execute_fill_login_fields(ICoreWebView2* sender,
                                      const std::string& username,
                                      const std::string& password)
{
    if (!sender) return;
    std::wstring script = L"__fillLoginFields('"
        + js_str_escape_w(username) + L"','"
        + js_str_escape_w(password) + L"');";
    sender->ExecuteScript(script.c_str(), nullptr);
}

static std::wstring make_accounts_init_script()
{
    auto accounts = accounts_load();
    std::wstring script = L"__initAccounts('";
    script += js_str_escape_w(g_config.selected_account);
    script += L"',[";
    for (size_t i = 0; i < accounts.size(); ++i) {
        if (i > 0) script += L',';
        script += L"'";
        script += js_str_escape_w(accounts[i].username);
        script += L"'";
    }
    script += L"]);";
    return script;
}

static std::wstring make_resolution_init_script()
{
    auto options = resolution_dropdown_options();
    std::wstring script = L"__initResolutionSettings([";
    for (size_t i = 0; i < options.size(); ++i) {
        if (i > 0) script += L',';
        script += L"'";
        script += js_str_escape_w(options[i]);
        script += L"'";
    }
    script += L"],'";
    script += js_str_escape_w(g_config.game_resolution);
    script += L"');";
    return script;
}

static bool should_autofill_account_footer()
{
    return g_config.remembered_username.empty()
        && !g_config.selected_account.empty();
}

static void refresh_accounts_ui(ICoreWebView2* sender, bool fill_footer)
{
    if (!sender) return;
    sender->ExecuteScript(make_accounts_init_script().c_str(), nullptr);
    if (fill_footer && !g_config.selected_account.empty()) {
        std::string pw = accounts_get_password(g_config.selected_account);
        execute_fill_login_fields(sender, g_config.selected_account, pw);
    }
}

// ---------------------------------------------------------------------------
// UI resource extraction
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

void extract_ui_resources()
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

// ---------------------------------------------------------------------------
// WebView2 controller creation and wiring
// ---------------------------------------------------------------------------
static void resize_webview(HWND hwnd)
{
    if (!g_controller) return;
    RECT bounds{};
    GetClientRect(hwnd, &bounds);
    g_controller->put_Bounds(bounds);
}

static void create_main_controller(ICoreWebView2Environment* env, HWND hwnd)
{
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

                // Lock RasterizationScale to 1.0 on high-DPI displays when the
                // window size is fixed, so 1 CSS pixel equals 1 physical pixel.
                if (g_config.fixed_window_size) {
                    ComPtr<ICoreWebView2Controller3> ctrl3;
                    if (SUCCEEDED(g_controller.As(&ctrl3))) {
                        ctrl3->put_ShouldDetectMonitorScaleChanges(FALSE);
                        ctrl3->put_RasterizationScale(1.0);
                    }
                }

                ComPtr<ICoreWebView2Settings> settings;
                g_webview->get_Settings(&settings);
                settings->put_AreDefaultContextMenusEnabled(FALSE);
                settings->put_IsStatusBarEnabled(FALSE);
                settings->put_AreDevToolsEnabled(FALSE);

                // One-shot NavigationCompleted: initialise the UI with stored config.
                auto token = std::make_shared<EventRegistrationToken>();
                g_webview->add_NavigationCompleted(
                    Callback<ICoreWebView2NavigationCompletedEventHandler>(
                        [token](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs*) mutable -> HRESULT
                        {
                            sender->remove_NavigationCompleted(*token);

                            std::wstring init_script =
                                L"__setRememberedUsername('"
                                + js_str_escape_w(g_config.remembered_username) + L"');";
                            sender->ExecuteScript(init_script.c_str(), nullptr);

                            if (g_config.remember_password && !g_config.remembered_password.empty()) {
                                std::string pw = crypto_dpapi_decrypt(g_config.remembered_password);
                                if (!pw.empty()) {
                                    std::wstring pw_script =
                                        L"__setRememberedPassword('"
                                        + js_str_escape_w(pw) + L"');";
                                    sender->ExecuteScript(pw_script.c_str(), nullptr);
                                } else {
                                    g_config.remembered_password.clear();
                                    config_save(g_config);
                                }
                            }

                            if (g_config.enable_accounts && should_autofill_account_footer()) {
                                std::string pw = accounts_get_password(g_config.selected_account);
                                execute_fill_login_fields(sender, g_config.selected_account, pw);
                            }

                            std::string starting_page = g_config.starting_page;
                            if (!g_config.enable_accounts && starting_page == "Accounts")
                                starting_page.clear();
                            std::wstring w_starting_page(starting_page.begin(),
                                                         starting_page.end());
                            std::wstring settings_script =
                                L"__initSettings('"
                                + js_str_escape_w(g_config.install_dir) + L"',"
                                + (g_config.fixed_window_size  ? L"true" : L"false") + L","
                                + (g_config.quick_launch       ? L"true" : L"false") + L","
                                + (g_config.use_orig_auth      ? L"true" : L"false") + L","
                                + (g_config.keep_open          ? L"true" : L"false") + L","
                                + (g_config.remember_password  ? L"true" : L"false") + L","
                                + (g_config.enable_accounts    ? L"true" : L"false") + L","
                                + L"'" + w_starting_page + L"');";
                            sender->ExecuteScript(settings_script.c_str(), nullptr);
                            sender->ExecuteScript(make_resolution_init_script().c_str(), nullptr);

                            if (g_config.enable_accounts)
                                sender->ExecuteScript(make_accounts_init_script().c_str(), nullptr);

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

                // Handle messages from JavaScript via window.chrome.webview.postMessage().
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
                                bool was_accounts_enabled = g_config.enable_accounts;
                                g_config.install_dir        = wstring_to_narrow(json_get_wstring(msg, L"install_dir"));
                                g_config.fixed_window_size  = json_get_wbool(msg, L"fixed_window_size");
                                g_config.quick_launch       = json_get_wbool(msg, L"quick_launch");
                                g_config.use_orig_auth      = json_get_wbool(msg, L"use_orig_auth");
                                g_config.keep_open          = json_get_wbool(msg, L"keep_open");
                                g_config.remember_password  = json_get_wbool(msg, L"remember_password");
                                g_config.enable_accounts    = json_get_wbool(msg, L"enable_accounts");
                                g_config.starting_page      = wstring_to_narrow(json_get_wstring(msg, L"starting_page"));
                                if (!g_config.enable_accounts && g_config.starting_page == "Accounts")
                                    g_config.starting_page.clear();
                                g_config.game_resolution = wstring_to_narrow(json_get_wstring(msg, L"game_resolution"));
                                if (g_config.game_resolution.empty())
                                    g_config.game_resolution = "off";
                                if (!is_valid_resolution_selection(g_config.game_resolution))
                                    g_config.game_resolution = "off";
                                if (!g_config.remember_password)
                                    g_config.remembered_password.clear();
                                config_save(g_config);
                                if (!was_accounts_enabled && g_config.enable_accounts)
                                    refresh_accounts_ui(g_webview.Get(), should_autofill_account_footer());
                                LOG_STAT("CORE_CONFIG", "Settings saved.");
                            }
                            else if (action == L"add_account") {
                                std::string u = wstring_to_narrow(json_get_wstring(msg, L"username"));
                                std::string p = wstring_to_narrow(json_get_wstring(msg, L"password"));
                                if (u.empty() || p.empty()) return S_OK;
                                AccountSaveResult result = accounts_add_or_update(u, p);
                                if (result == AccountSaveResult::Failed) return S_OK;
                                if (result == AccountSaveResult::Added)
                                    LOG_STAT("CORE_ACCOUNTS", "account added: " + u);
                                else
                                    LOG_STAT("CORE_ACCOUNTS", "account updated: " + u);
                                bool fill = g_config.selected_account.empty();
                                if (fill)
                                    g_config.selected_account = u;
                                else if (g_config.selected_account == u)
                                    fill = true;
                                config_save(g_config);
                                refresh_accounts_ui(g_webview.Get(), fill);
                            }
                            else if (action == L"remove_account") {
                                std::string u = wstring_to_narrow(json_get_wstring(msg, L"username"));
                                if (u.empty()) return S_OK;
                                accounts_remove(u);
                                LOG_STAT("CORE_ACCOUNTS", "account removed: " + u);
                                bool fill = false;
                                if (g_config.selected_account == u) {
                                    auto remaining = accounts_load();
                                    if (remaining.empty()) {
                                        g_config.selected_account.clear();
                                        execute_fill_login_fields(g_webview.Get(), "", "");
                                    } else {
                                        g_config.selected_account = remaining[0].username;
                                        fill = true;
                                    }
                                    config_save(g_config);
                                }
                                refresh_accounts_ui(g_webview.Get(), fill);
                            }
                            else if (action == L"select_account") {
                                std::string u = wstring_to_narrow(json_get_wstring(msg, L"username"));
                                if (u.empty()) return S_OK;
                                g_config.selected_account = u;
                                config_save(g_config);
                                LOG_STAT("CORE_ACCOUNTS", "account selected: " + u);
                                std::string pw = accounts_get_password(u);
                                execute_fill_login_fields(g_webview.Get(), u, pw);
                            }
                            else if (action == L"play") {
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

                // Open http/https links in the system default browser.
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
// Public API
// ---------------------------------------------------------------------------
void init_webview(HWND hwnd)
{
    // Strip the trailing backslash: WebView2 requires no trailing separator.
    std::wstring user_data = g_ui_base;
    if (!user_data.empty() && user_data.back() == L'\\')
        user_data.pop_back();

    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, user_data.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT
            {
                if (FAILED(hr) || !env) return hr;

                // Start background news fetch; result arrives via WM_NEWS_READY.
                news_start_fetch(hwnd);

                create_main_controller(env, hwnd);

                return S_OK;
            }
        ).Get()
    );
}

void webview_execute(const wchar_t* script)
{
    if (g_webview)
        g_webview->ExecuteScript(script, nullptr);
}

void webview_on_news_ready()
{
    g_news_ready = true;
    if (g_nav_complete && g_webview) {
        LOG_STAT("CORE_NEWS", "ui already loaded : running extraction script");
        g_webview->ExecuteScript(news_make_script().c_str(), nullptr);
    } else {
        LOG_STAT("CORE_NEWS", "ui not yet loaded : will render in NavigationCompleted");
    }
}
