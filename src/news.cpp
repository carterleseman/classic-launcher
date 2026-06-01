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
    // User-Agent must match a recent Chrome/Edge build : Akamai's WAF checks it
    // against the Sec-CH-UA version token and blocks stale/non-browser agents.
    HINTERNET session = WinHttpOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        L"(KHTML, like Gecko) Chrome/148.0.0.0 Safari/537.36 Edg/148.0.0.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        LOG_ERR("CORE_NEWS", "WinHttpOpen failed: " + std::to_string(GetLastError()));
        return {};
    }

    HINTERNET conn = WinHttpConnect(session, L"www.wizard101.com",
                                    INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) {
        LOG_ERR("CORE_NEWS", "WinHttpConnect failed: " + std::to_string(GetLastError()));
        WinHttpCloseHandle(session);
        return {};
    }

    HINTERNET req = WinHttpOpenRequest(conn, L"GET", L"/game/news",
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

    // Akamai WAF requires Sec-Fetch-* headers to accept the request as a
    // legitimate browser navigation. Without them it returns 403.
    // Referer must be within wizard101.com for Sec-Fetch-Site: same-origin.
    static const wchar_t* HEADERS =
        L"Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7\r\n"
        L"Accept-Language: en-US,en;q=0.9\r\n"
        L"Cache-Control: max-age=0\r\n"
        L"Cookie: Login1=1; userLocale=en\r\n"
        L"Priority: u=0, i\r\n"
        L"Referer: https://www.wizard101.com/game/worlds\r\n"
        L"Sec-CH-UA: \"Chromium\";v=\"148\", \"Microsoft Edge\";v=\"148\", \"Not/A)Brand\";v=\"99\"\r\n"
        L"Sec-CH-UA-Mobile: ?0\r\n"
        L"Sec-CH-UA-Platform: \"Windows\"\r\n"
        L"Sec-Fetch-Dest: document\r\n"
        L"Sec-Fetch-Mode: navigate\r\n"
        L"Sec-Fetch-Site: same-origin\r\n"
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

    LOG_STAT("CORE_NEWS", "HTTP status: " + std::to_string(status));

    if (status != 200) {
        LOG_WARN("CORE_NEWS", "Unexpected HTTP status : aborting fetch");
        cleanup(); return {};
    }

    std::string html;
    std::vector<char> buf(65536);
    DWORD bytes_read = 0;
    while (WinHttpReadData(req, buf.data(), static_cast<DWORD>(buf.size()), &bytes_read)
           && bytes_read > 0)
        html.insert(html.end(), buf.begin(), buf.begin() + bytes_read);

    LOG_STAT("CORE_NEWS", "Fetched " + std::to_string(html.size()) + " bytes");

    cleanup();
    return html;
}

// ---------------------------------------------------------------------------
//  Extraction script : injected into the main WebView2 after ui.html loads.
//
//  The script:
//    1. Decodes the base64 UTF-8 HTML passed as an argument.
//    2. Parses it into a virtual document with DOMParser.
//    3. Sets a <base> so relative URLs resolve against www.wizard101.com.
//    4. Runs the same DOM selectors as the old hidden-WebView2 script.
//    5. Calls __renderNews(results) directly : no round-trip to C++ needed.
// ---------------------------------------------------------------------------
static const wchar_t* SCRIPT_PREFIX = LR"js(
(function(b64) {
  var bin = atob(b64), bytes = new Uint8Array(bin.length);
  for (var i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  var html = new TextDecoder('utf-8').decode(bytes);

  var doc = new DOMParser().parseFromString(html, 'text/html');
  var base = doc.createElement('base');
  base.href = 'https://www.wizard101.com';
  if (doc.head) doc.head.insertBefore(base, doc.head.firstChild);

  var results = [];
  var tds = doc.querySelectorAll('td.maincontentpattern > table > tbody > tr > td');
  var nonEmpty = 0;

  for (var i = 0; i < tds.length; i++) {
    var td = tds[i];
    if (!td.innerHTML.trim()) continue;
    nonEmpty++;
    if (nonEmpty <= 2) continue;

    var item = { date: '', img_href: null, img_src: '', img_w: 0, img_h: 0, body: '' };
    var contentbox = td.querySelector('div.contentbox');
    if (!contentbox) { results.push(item); continue; }

    var tables = contentbox.querySelectorAll(':scope > table');
    if (tables.length < 2) { results.push(item); continue; }

    var h2 = tables[0].querySelector('tbody > tr > td.contentbox_headermiddle > h2');
    if (h2) item.date = h2.textContent.trim();

    var bodyRows = tables[1].rows;
    if (bodyRows.length > 1) {
      var bodyCells = bodyRows[1].cells;
      if (bodyCells.length > 1) {
        var cell = bodyCells[1];
        var anchor = cell.querySelector('div > a');
        if (anchor) {
          item.img_href = anchor.href || null;
          var aImg = anchor.querySelector('img');
          if (aImg) {
            item.img_src = aImg.src || '';
            item.img_w   = aImg.width  || 0;
            item.img_h   = aImg.height || 0;
          }
        } else {
          var dImg = cell.querySelector('div > img');
          if (dImg) {
            item.img_src = dImg.src || '';
            item.img_w   = dImg.width  || 0;
            item.img_h   = dImg.height || 0;
          }
        }
        var p = cell.querySelector('p');
        if (p) item.body = p.innerHTML || '';
      }
    }
    results.push(item);
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
    LOG_STAT("CORE_NEWS", "Starting background fetch...");
    std::thread([hwnd]() {
        std::string html = fetch_news_html();
        if (!html.empty()) {
            g_b64html = base64_encode(html);
            LOG_STAT("CORE_NEWS", "Encoded to " + std::to_string(g_b64html.size()) + " base64 chars");
        } else {
            LOG_WARN("CORE_NEWS", "Fetch returned empty : will render empty news");
        }
        PostMessage(hwnd, WM_NEWS_READY, 0, 0);
    }).detach();
}

std::wstring news_make_script()
{
    if (g_b64html.empty()) {
        LOG_WARN("CORE_NEWS", "news_make_script: no HTML, returning empty render");
        return L"if (typeof __renderNews === 'function') __renderNews([]);";
    }
    LOG_STAT("CORE_NEWS", "news_make_script: building DOMParser extraction script");
    std::wstring b64(g_b64html.begin(), g_b64html.end());
    return std::wstring(SCRIPT_PREFIX) + b64 + std::wstring(SCRIPT_SUFFIX);
}
