#define WIN32_LEAN_AND_MEAN
#include "news.h"
#include "logger.h"

#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>
#include <thread>

#pragma comment(lib, "winhttp.lib")

// Base64-encoded HTML from the last successful fetch.
// Written once by the background thread before WM_NEWS_READY is posted.
static std::string g_b64html;

// ---------------------------------------------------------------------------
//  base64_encode : standard Base64, no line breaks.
// ---------------------------------------------------------------------------
static std::string base64_encode(const std::string& in)
{
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        uint32_t v = static_cast<uint8_t>(in[i]) << 16;
        if (i + 1 < in.size()) v |= static_cast<uint8_t>(in[i + 1]) << 8;
        if (i + 2 < in.size()) v |= static_cast<uint8_t>(in[i + 2]);
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += (i + 1 < in.size()) ? T[(v >>  6) & 63] : '=';
        out += (i + 2 < in.size()) ? T[ v        & 63] : '=';
    }
    return out;
}

// ---------------------------------------------------------------------------
//  fetch_news_html : synchronous WinHTTP HTTPS GET.
//  Returns the raw UTF-8 HTML body, or an empty string on any error.
// ---------------------------------------------------------------------------
static std::string fetch_news_html()
{
    // User-Agent should match the Sec-CH-UA version. The news host is
    // www.play.wizard101.com; a direct navigation sends no Referer.
    HINTERNET session = WinHttpOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        L"(KHTML, like Gecko) Chrome/156.0.0.0 Safari/537.36",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        LOG_ERR("CORE_NEWS", "WinHttpOpen failed: " + std::to_string(GetLastError()));
        return {};
    }

    HINTERNET conn = WinHttpConnect(session, L"www.play.wizard101.com",
                                    INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) {
        LOG_ERR("CORE_NEWS", "WinHttpConnect failed: " + std::to_string(GetLastError()));
        WinHttpCloseHandle(session);
        return {};
    }

    HINTERNET req = WinHttpOpenRequest(conn, L"GET", L"/news",
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!req) {
        LOG_ERR("CORE_NEWS", "WinHttpOpenRequest failed: " + std::to_string(GetLastError()));
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return {};
    }

    auto cleanup = [&]() {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
    };

    // Direct navigation to /news. Accept-Encoding is omitted so the body stays
    // plain HTML; WinHTTP does not decompress brotli or zstd.
    static const wchar_t* HEADERS =
        L"Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7\r\n"
        L"Accept-Language: en-US,en;q=0.9\r\n"
        L"Cookie: Login1=1; userLocale=en\r\n"
        L"Priority: u=0, i\r\n"
        L"Sec-CH-UA: \"Not:A-Brand\";v=\"8\", \"Chromium\";v=\"156\", \"Google Chrome\";v=\"156\"\r\n"
        L"Sec-CH-UA-Mobile: ?0\r\n"
        L"Sec-CH-UA-Platform: \"Windows\"\r\n"
        L"Sec-Fetch-Dest: document\r\n"
        L"Sec-Fetch-Mode: navigate\r\n"
        L"Sec-Fetch-Site: none\r\n"
        L"Sec-Fetch-User: ?1\r\n"
        L"Upgrade-Insecure-Requests: 1\r\n";

    WinHttpAddRequestHeaders(req, HEADERS, static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        LOG_ERR("CORE_NEWS", "WinHttpSendRequest failed: " + std::to_string(GetLastError()));
        cleanup(); return {};
    }

    if (!WinHttpReceiveResponse(req, nullptr)) {
        LOG_ERR("CORE_NEWS", "WinHttpReceiveResponse failed: " + std::to_string(GetLastError()));
        cleanup(); return {};
    }

    DWORD status = 0;
    {
        DWORD len = sizeof(status);
        WinHttpQueryHeaders(req,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    }

    LOG_STAT("CORE_NEWS", "news http status: " + std::to_string(status));

    if (status != 200) {
        LOG_WARN("CORE_NEWS", "unexpected http status : aborting fetch");
        cleanup(); return {};
    }

    std::string html;
    std::vector<char> buf(65536);
    DWORD bytes_read = 0;
    while (WinHttpReadData(req, buf.data(), static_cast<DWORD>(buf.size()), &bytes_read)
           && bytes_read > 0)
        html.insert(html.end(), buf.begin(), buf.begin() + bytes_read);

    cleanup();
    return html;
}

// ---------------------------------------------------------------------------
//  Extraction script : injected into the main WebView2 after ui.html loads.
//
//  The script:
//    1. Decodes the base64 UTF-8 HTML passed as an argument.
//    2. Parses it into a virtual document with DOMParser.
//    3. Sets a <base> so relative URLs resolve against www.play.wizard101.com.
//    4. Collects every app-content-news-story-preview on the page.
//    5. Calls __renderNews(results) directly : no round-trip to C++ needed.
// ---------------------------------------------------------------------------
static const wchar_t* SCRIPT_PREFIX = LR"js(
(function(b64) {
  var bin = atob(b64), bytes = new Uint8Array(bin.length);
  for (var i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  var html = new TextDecoder('utf-8').decode(bytes);

  var doc = new DOMParser().parseFromString(html, 'text/html');
  var base = doc.createElement('base');
  base.href = 'https://www.play.wizard101.com/news?tags=PC,NA';
  if (doc.head) doc.head.insertBefore(base, doc.head.firstChild);

  var results = [];
  var stories = doc.querySelectorAll('app-content-news-story-preview');

  for (var i = 0; i < stories.length; i++) {
    var story = stories[i];
    var date = story.querySelector('.news-preview-date');
    var img = story.querySelector('img.news-preview-image');
    var title = story.querySelector('.news-preview-title');
    var text = story.querySelector('.news-preview-text');
    var link = story.querySelector('app-content-button a');
    var tagNodes = story.querySelectorAll('.news-preview-tag');
    var tags = [];
    for (var t = 0; t < tagNodes.length; t++) {
      var tag = tagNodes[t].getAttribute('title') || tagNodes[t].textContent.trim();
      if (tag) tags.push(tag);
    }

    results.push({
      date: date ? (date.getAttribute('datetime')) : '',
      img_src: img ? img.src : '',
      title: title ? title.textContent.trim() : '',
      text: text ? text.textContent.trim() : '',
      href: link ? link.href : '',
      tags: tags
    });
  }

  if (typeof __renderNews === 'function') __renderNews(results);
})(')js";

static const wchar_t* SCRIPT_SUFFIX = LR"js(')
)js";

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------

void news_start_fetch(HWND hwnd)
{
    LOG_STAT("CORE_NEWS", "starting news fetch...");
    std::thread([hwnd]() {
        std::string html = fetch_news_html();
        if (!html.empty()) {
            g_b64html = base64_encode(html);
        } else {
            LOG_WARN("CORE_NEWS", "fetch returned empty : will render empty news");
        }
        PostMessage(hwnd, WM_NEWS_READY, 0, 0);
    }).detach();
}

std::wstring news_make_script()
{
    if (g_b64html.empty()) {
        LOG_WARN("CORE_NEWS", "news_make_script: no html, returning empty render");
        return L"if (typeof __renderNews === 'function') __renderNews([]);";
    }
    std::wstring b64(g_b64html.begin(), g_b64html.end());
    return std::wstring(SCRIPT_PREFIX) + b64 + std::wstring(SCRIPT_SUFFIX);
}
