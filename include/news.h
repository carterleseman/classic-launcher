#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>

// Posted to hwnd (on the UI thread) when the WinHTTP news fetch completes.
// WndProc should handle this message and call news_make_script() to get the
// ExecuteScript content that extracts and renders the news via DOMParser.
#define WM_NEWS_READY (WM_APP + 5)

// Starts a background thread that fetches https://www.wizard101.com/game/news
// via WinHTTP (Cookie: Login1=1) and base64-encodes the response HTML.
// Posts WM_NEWS_READY to hwnd when done (or on failure : news_make_script
// returns a no-op script in that case).
void news_start_fetch(HWND hwnd);

// Returns the ExecuteScript content for the main WebView2.
// The script decodes the fetched HTML, parses it with DOMParser, runs the
// news extraction selectors, and calls __renderNews(results) directly.
// Returns a benign no-op if the fetch hasn't completed or failed.
std::wstring news_make_script();
