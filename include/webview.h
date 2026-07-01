#pragma once
#include <windows.h>

// ---------------------------------------------------------------------------
//  WebView2 initialisation and embedded UI resource extraction.
//
//  Call order from wWinMain:
//    1. extract_ui_resources()  : extracts assets before WebView2 starts
//    2. init_webview(hwnd)      : begins async WebView2 environment + controller
//
//  From WndProc:
//    webview_execute(script)    : forward a JS snippet to the live WebView
//    webview_on_news_ready()    : call on WM_NEWS_READY
// ---------------------------------------------------------------------------

// Extracts all embedded UI assets to %APPDATA%\wizlauncher\.
void extract_ui_resources();

// Starts async WebView2 environment and controller creation, and kicks off
// the background news fetch.
void init_webview(HWND hwnd);

// Executes a JS snippet in the WebView. No-op if the WebView is not yet ready.
void webview_execute(const wchar_t* script);

// Signals that the background news fetch completed.
void webview_on_news_ready();
