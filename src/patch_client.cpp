#include "patch_client.h"
#include "dml.h"
#include "logger.h"

#include <unordered_map>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <stdexcept>
#include <fstream>
#include <sstream>
#include <vector>
#include <cstring>
#include <cctype>

#include "zlib.h"

#pragma comment(lib, "winhttp.lib")

// ---------------------------------------------------------------------------
//  gz_decompress : decompress a gzip buffer using zlib.
//  windowBits = 15 + 16 tells zlib to auto-detect and handle the gzip wrapper.
//  Returns the decompressed bytes, or an empty vector on any error.
// ---------------------------------------------------------------------------
static std::vector<uint8_t> gz_decompress(const uint8_t* gz, size_t gz_len)
{
    z_stream strm{};
    // 15 + 16 = automatic gzip header detection and decoding.
    if (inflateInit2(&strm, 15 + 16) != Z_OK) return {};

    strm.next_in  = const_cast<Bytef*>(gz);
    strm.avail_in = static_cast<uInt>(gz_len);

    std::vector<uint8_t> out;
    std::vector<uint8_t> buf(65536);
    int ret;
    do {
        strm.next_out  = buf.data();
        strm.avail_out = static_cast<uInt>(buf.size());
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
            inflateEnd(&strm);
            return {};
        }
        out.insert(out.end(), buf.begin(), buf.begin() + (buf.size() - strm.avail_out));
    } while (ret != Z_STREAM_END);

    inflateEnd(&strm);
    return out;
}

// ---------------------------------------------------------------------------
//  Internal helpers shared by PatchHttpClient and patch_download_file.
// ---------------------------------------------------------------------------
static void make_parent_dirs(const std::string& path)
{
    std::string dir = path;
    auto slash = dir.find_last_of("/\\");
    if (slash == std::string::npos) return;
    dir.resize(slash);
    for (size_t i = 0; i <= dir.size(); i++) {
        if (i == dir.size() || dir[i] == '/' || dir[i] == '\\') {
            std::string partial(dir.begin(), dir.begin() + i);
            if (!partial.empty())
                CreateDirectoryA(partial.c_str(), nullptr);
        }
    }
}

static int http_pct(uint64_t done, uint64_t total)
{
    if (total == 0) return 0;
    int p = static_cast<int>(done * 100 / total);
    return p < 100 ? p : 100;
}

// ---------------------------------------------------------------------------
//  PatchHttpClient : owns one WinHTTP session + connection.
//
//  All file downloads in a single patch run share this object so there is
//  only one DNS lookup and one underlying TCP connection for the entire run.
//  reconnect() drops and reopens only the connection handle, keeping the
//  session alive, which is enough to recover from a server-side reset (12030)
//  without triggering another DNS query (12007).
// ---------------------------------------------------------------------------
enum class GetResult { Downloaded, Skipped, Retriable };

struct PatchHttpClient {
    HINTERNET            session   = nullptr;
    HINTERNET            conn      = nullptr;
    std::vector<wchar_t> host_buf;
    INTERNET_PORT        port      = INTERNET_DEFAULT_HTTP_PORT;
    DWORD                req_flags = 0; // WINHTTP_FLAG_SECURE or 0

    explicit PatchHttpClient(const std::string& any_cdn_url)
        : host_buf(256, L'\0')
    {
        std::wstring wurl(any_cdn_url.begin(), any_cdn_url.end());
        auto scheme_buf = std::vector<wchar_t>(16, L'\0');
        URL_COMPONENTS uc{};
        uc.dwStructSize     = sizeof(uc);
        uc.lpszScheme       = scheme_buf.data(); uc.dwSchemeLength   = static_cast<DWORD>(scheme_buf.size());
        uc.lpszHostName     = host_buf.data();   uc.dwHostNameLength = static_cast<DWORD>(host_buf.size());
        if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc))
            throw std::runtime_error("PatchHttpClient: invalid URL: " + any_cdn_url);

        port      = uc.nPort;
        req_flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;

        session = WinHttpOpen(L"WizLauncher/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)
            throw std::runtime_error("PatchHttpClient: WinHttpOpen failed (err="
                                     + std::to_string(GetLastError()) + ")");

        conn = WinHttpConnect(session, host_buf.data(), port, 0);
        if (!conn) {
            DWORD e = GetLastError();
            WinHttpCloseHandle(session); session = nullptr;
            throw std::runtime_error("PatchHttpClient: WinHttpConnect failed (err="
                                     + std::to_string(e) + ")");
        }
    }

    ~PatchHttpClient()
    {
        if (conn)    WinHttpCloseHandle(conn);
        if (session) WinHttpCloseHandle(session);
    }

    PatchHttpClient(const PatchHttpClient&)            = delete;
    PatchHttpClient& operator=(const PatchHttpClient&) = delete;

    // Drop and reopen the connection : recovers from 12030 without a new DNS query.
    void reconnect()
    {
        if (conn) { WinHttpCloseHandle(conn); conn = nullptr; }
        conn = WinHttpConnect(session, host_buf.data(), port, 0);
        if (!conn)
            throw std::runtime_error("PatchHttpClient::reconnect failed (err="
                                     + std::to_string(GetLastError()) + ")");
    }

    // Download a URL into memory. Returns empty vector on 403/404/error.
    // Used to fetch the .gz variant before decompressing.
    std::vector<uint8_t> download_to_memory(const std::string& url)
    {
        std::wstring wurl(url.begin(), url.end());
        auto path_buf = std::vector<wchar_t>(2048, L'\0');
        URL_COMPONENTS uc{};
        uc.dwStructSize    = sizeof(uc);
        uc.lpszUrlPath     = path_buf.data();
        uc.dwUrlPathLength = static_cast<DWORD>(path_buf.size());
        if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return {};

        HINTERNET req = WinHttpOpenRequest(conn, L"GET", path_buf.data(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, req_flags);
        if (!req) return {};

        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            || !WinHttpReceiveResponse(req, nullptr)) {
            WinHttpCloseHandle(req);
            return {};
        }

        DWORD status = 0;
        {
            DWORD len = sizeof(status);
            WinHttpQueryHeaders(req,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
        }
        if (status != 200) { WinHttpCloseHandle(req); return {}; }

        DWORD content_length = 0;
        {
            DWORD len = sizeof(content_length);
            WinHttpQueryHeaders(req,
                WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &content_length, &len, WINHTTP_NO_HEADER_INDEX);
        }

        std::vector<uint8_t> buf;
        if (content_length > 0) buf.reserve(content_length);

        std::vector<uint8_t> chunk(65536);
        DWORD bytes_read = 0;
        while (WinHttpReadData(req, chunk.data(), static_cast<DWORD>(chunk.size()), &bytes_read)
               && bytes_read > 0)
            buf.insert(buf.end(), chunk.begin(), chunk.begin() + bytes_read);

        WinHttpCloseHandle(req);
        return buf;
    }

    // Download url to local_path.
    //   Downloaded : file written successfully.
    //   Skipped    : server returned 403 or 404; file not written, not an error.
    //   Retriable  : transport failure; caller should reconnect() and retry.
    // Hard errors (unexpected HTTP status, can't create file) throw.
    GetResult get(const std::string& url,
                  const std::string& local_path,
                  const std::string& display_name,
                  PatchFileProgressFn on_progress,
                  std::string& out_err)
    {
        std::wstring wurl(url.begin(), url.end());
        auto path_buf = std::vector<wchar_t>(2048, L'\0');
        URL_COMPONENTS uc{};
        uc.dwStructSize    = sizeof(uc);
        uc.lpszUrlPath     = path_buf.data();
        uc.dwUrlPathLength = static_cast<DWORD>(path_buf.size());
        if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
            out_err = "invalid URL: " + url;
            return GetResult::Retriable;
        }

        HINTERNET req = WinHttpOpenRequest(conn, L"GET", path_buf.data(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, req_flags);
        if (!req) {
            out_err = "WinHttpOpenRequest failed (err=" + std::to_string(GetLastError()) + ")";
            return GetResult::Retriable;
        }

        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            || !WinHttpReceiveResponse(req, nullptr)) {
            out_err = "send/receive failed (err=" + std::to_string(GetLastError()) + ") for " + url;
            WinHttpCloseHandle(req);
            return GetResult::Retriable;
        }

        DWORD status = 0;
        {
            DWORD len = sizeof(status);
            WinHttpQueryHeaders(req,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                WINHTTP_NO_HEADER_INDEX);
        }
        if (status == 403 || status == 404) {
            WinHttpCloseHandle(req);

            // For .exe and .dll, the CDN blocks the raw path but serves a
            // gzip-compressed copy at src_name + ".gz". Try that before giving up.
            bool try_gz = false;
            {
                size_t dot = url.rfind('.');
                if (dot != std::string::npos) {
                    std::string ext = url.substr(dot);
                    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    try_gz = (ext == ".exe" || ext == ".dll");
                }
            }

            if (try_gz) {
                std::string gz_url = url + ".gz";
                LOG_WARN("PATCH_CLIENT", "403 on raw URL, trying: " + gz_url);

                auto compressed = download_to_memory(gz_url);
                if (!compressed.empty()) {
                    auto decompressed = gz_decompress(compressed.data(), compressed.size());
                    if (!decompressed.empty()) {
                        std::ofstream out(local_path, std::ios::binary | std::ios::trunc);
                        if (out) {
                            out.write(reinterpret_cast<const char*>(decompressed.data()),
                                      static_cast<std::streamsize>(decompressed.size()));
                            if (on_progress) on_progress(display_name, 100);
                            LOG_STAT("PATCH_CLIENT", local_path + " (via .gz, "
                                + std::to_string(decompressed.size()) + " bytes)");
                            return GetResult::Downloaded;
                        }
                    } else {
                        LOG_ERR("PATCH_CLIENT", "gz decompress failed for: " + gz_url);
                    }
                } else {
                    LOG_WARN("PATCH_CLIENT", ".gz also failed for: " + url);
                }
            }

            LOG_WARN("PATCH_CLIENT", "skipping HTTP " + std::to_string(status) + ": " + url);
            return GetResult::Skipped;
        }
        if (status != 200) {
            WinHttpCloseHandle(req);
            throw std::runtime_error("patch: server returned HTTP "
                                     + std::to_string(status) + " for " + url);
        }

        uint64_t content_length = 0;
        {
            DWORD cl = 0, len = sizeof(cl);
            if (WinHttpQueryHeaders(req,
                    WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &cl, &len,
                    WINHTTP_NO_HEADER_INDEX))
                content_length = static_cast<uint64_t>(cl);
        }

        std::ofstream out(local_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            WinHttpCloseHandle(req);
            throw std::runtime_error("patch: cannot create " + local_path);
        }

        if (on_progress) on_progress(display_name, 0);

        std::vector<char> buf(65536);
        DWORD    bytes_read = 0;
        uint64_t bytes_done = 0;
        while (WinHttpReadData(req, buf.data(), static_cast<DWORD>(buf.size()), &bytes_read)
               && bytes_read > 0) {
            out.write(buf.data(), bytes_read);
            bytes_done += bytes_read;
            if (on_progress)
                on_progress(display_name, http_pct(bytes_done, content_length));
        }

        WinHttpCloseHandle(req);

        if (bytes_done == 0 && content_length > 0) {
            out_err = "read returned 0 bytes for " + url;
            return GetResult::Retriable;
        }

        if (on_progress) on_progress(display_name, 100);
        return GetResult::Downloaded;
    }
};

// ---------------------------------------------------------------------------

PatchInfo patch_query(const char* host, uint16_t port) {
    SOCKET s = dml_connect(host, port);

    try {
        auto offer_raw = dml_recv(s);
        (void)parse_session_offer(offer_raw);

        DmlWriter w;
        w.write_uint(0);         // LatestVersion
        w.write_str("");         // ListFileName
        w.write_uint(0);         // ListFileType
        w.write_uint(0);         // ListFileTime
        w.write_uint(1);         // ListFileSize
        w.write_uint(0);         // ListFileCRC
        w.write_str("");         // ListFileURL
        w.write_str("");         // URLPrefix
        w.write_str("");         // URLSuffix
        w.write_str("English");  // Locale

        auto req_pkt = build_dml_packet(SVC_PATCH, MSG_LATEST_FILE_LIST_V2, w.data());
        dml_send(s, req_pkt);

        auto rsp_raw = dml_recv(s);
        DmlPacket rsp = parse_dml_packet(rsp_raw);

        if (rsp.is_control)
            throw std::runtime_error("patch_query: expected DML response, got control packet");
        if (rsp.svc_id != SVC_PATCH || rsp.msg_type != MSG_LATEST_FILE_LIST_V2)
            throw std::runtime_error("patch_query: unexpected message svc="
                                     + std::to_string(rsp.svc_id)
                                     + " msg=" + std::to_string(rsp.msg_type));

        DmlReader r(rsp.data);
        PatchInfo info{};
        info.latest_version = r.read_uint();
        r.read_str();                          // ListFileName (ignored)
        r.read_uint();                         // ListFileType (ignored)
        r.read_uint();                         // ListFileTime (ignored)
        info.list_file_size = r.read_uint();
        info.list_file_crc  = r.read_uint();
        info.list_file_url  = r.read_str();
        info.url_prefix     = r.read_str();
        info.url_suffix     = r.read_str();

        dml_close(s);
        return info;
    }
    catch (...) {
        dml_close(s);
        throw;
    }
}

// ---------------------------------------------------------------------------
//  patch_download_file : download a URL to a local path via WinHTTP.
//  Creates parent directories as needed. Always overwrites if present.
//  Uses PatchHttpClient internally; retries up to 3 times with back-off.
// ---------------------------------------------------------------------------
bool patch_download_file(const std::string&   url,
                         const std::string&   local_path,
                         const std::string&   display_name,
                         PatchFileProgressFn  on_progress)
{
    make_parent_dirs(local_path);

    const int MAX_ATTEMPTS = 3;
    std::string last_err;

    for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
        if (attempt > 0)
            Sleep(static_cast<DWORD>(1000 * attempt));

        try {
            PatchHttpClient client(url);
            auto result = client.get(url, local_path, display_name, on_progress, last_err);
            if (result == GetResult::Downloaded) return true;
            if (result == GetResult::Skipped)    return false;
            // Retriable : fall through to log and loop.
        } catch (const std::runtime_error&) {
            throw; // hard error (unexpected HTTP status, can't create file)
        }

        LOG_WARN("PATCH_CLIENT", "attempt " + std::to_string(attempt + 1)
                                 + " failed: " + last_err);
    }

    throw std::runtime_error("patch_download_file: " + last_err);
}

// ---------------------------------------------------------------------------
//  Little-endian helpers for LatestFileList.bin parser.
// ---------------------------------------------------------------------------
static uint16_t read_u16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t read_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static std::string read_str_le(const uint8_t*& p, const uint8_t* end) {
    if (p + 2 > end) throw std::runtime_error("LatestFileList.bin: truncated string length");
    uint16_t len = read_u16(p); p += 2;
    if (p + len > end) throw std::runtime_error("LatestFileList.bin: truncated string data");
    std::string s(reinterpret_cast<const char*>(p), len);
    p += len;
    return s;
}
static void skip_dml_record(const uint8_t*& p, const uint8_t* end) {
    if (p + 4 > end) throw std::runtime_error("LatestFileList.bin: truncated record header");
    uint16_t size = read_u16(p + 2);
    p += size;
}

// ---------------------------------------------------------------------------
//  patch_get_file_list : always downloads a fresh LatestFileList.bin, then
//  parses it into PatchFile records.
// ---------------------------------------------------------------------------
std::vector<PatchFile> patch_get_file_list(const PatchInfo& info,
                                           const std::string& local_path)
{
    patch_download_file(info.list_file_url, local_path);

    std::ifstream f(local_path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("patch_get_file_list: cannot open " + local_path);
    auto file_size = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> buf(file_size);
    f.read(reinterpret_cast<char*>(buf.data()), file_size);

    const uint8_t* p   = buf.data();
    const uint8_t* end = p + file_size;

    if (p + 4 > end) throw std::runtime_error("LatestFileList.bin: file too short");
    uint32_t hdr_count = read_u32(p); p += 4;

    for (uint32_t i = 0; i <= hdr_count; i++)
        skip_dml_record(p, end);

    if (p + 4 > end) throw std::runtime_error("LatestFileList.bin: truncated version");
    p += 4; // skip version i32

    skip_dml_record(p, end);

    if (p + 12 > end) throw std::runtime_error("LatestFileList.bin: truncated unknown block");
    p += 12;

    std::vector<PatchFile> records;
    bool toggle = false;

    while (p < end) {
        if (p + 4 > end) break;

        if (!toggle) {
            // Each pre-toggle entry is wrapped in a schema record followed by
            // a 4-byte data record header (protocol_id + record_type + size).
            skip_dml_record(p, end);
            if (p + 4 > end) break;
            p += 4; // skip the data record header
        } else {
            // In toggle mode entries are packed directly; a MSG_CUSTOMDICT
            // record (0x02, 0x01) may occasionally appear between entries.
            if (p + 2 <= end && p[0] == 0x02 && p[1] == 0x01) {
                skip_dml_record(p, end); // skip the dict record
                if (p + 4 > end) break;
                p += 4; // dict is followed by a data record header as well
            }
        }

        PatchFile pf{};
        pf.src_name        = read_str_le(p, end);
        pf.tar_name        = read_str_le(p, end);
        if (p + 28 > end) break; // 6 known u32s (24) + 1 trailing u32 (4)
        pf.file_type       = read_u32(p); p += 4;
        pf.size            = read_u32(p); p += 4;
        pf.header_size     = read_u32(p); p += 4;
        pf.compressed_size = read_u32(p); p += 4;
        pf.crc             = read_u32(p); p += 4;
        pf.header_crc      = read_u32(p); p += 4;
        p += 4; // trailing u32 present in every entry

        // Skip entries with no CDN path.  PatchClient files are excluded from
        // the game patch except LoginMessages.xml, which is fetched separately
        // into AppData (see patch_update_login_messages).
        if (!pf.src_name.empty()) {
            const bool patchclient = pf.src_name.find("PatchClient") != std::string::npos;
            const bool login_msgs  = patchclient
                && pf.src_name.find("LoginMessages.xml") != std::string::npos;
            if (!patchclient || login_msgs)
                records.push_back(pf);
        }

        if (!toggle && pf.src_name.find("Bin/") != std::string::npos)
            toggle = true;
    }

    return records;
}

// ---------------------------------------------------------------------------
//  JSON cache : %APPDATA%\wizlauncher\patch_cache.json
//
//  Format: flat JSON object  { "tar/name.wad": <crc_u32>, ... }
// ---------------------------------------------------------------------------

std::string patch_default_cache_path()
{
    char appdata[MAX_PATH]{};
    if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH) == 0)
        throw std::runtime_error("patch_default_cache_path: APPDATA not set");
    return std::string(appdata) + "\\wizlauncher\\patch_cache.json";
}

// ---------------------------------------------------------------------------
//  Minimal JSON writer for { "key": uint32, ... }.
// ---------------------------------------------------------------------------
void patch_cache_save(const std::string& cache_path, const PatchCache& cache)
{
    auto sep = cache_path.find_last_of("/\\");
    if (sep != std::string::npos) {
        std::string dir(cache_path.begin(), cache_path.begin() + sep);
        CreateDirectoryA(dir.c_str(), nullptr);
    }

    std::ofstream f(cache_path, std::ios::trunc);
    if (!f)
        throw std::runtime_error("patch_cache_save: cannot write " + cache_path);

    f << "{\n";
    bool first = true;
    for (const auto& kv : cache) {
        if (!first) f << ",\n";
        // Normalise any backslashes in the key to forward slashes.
        std::string key = kv.first;
        for (auto& c : key) if (c == '\\') c = '/';
        f << "  \"" << key << "\": " << kv.second;
        first = false;
    }
    f << "\n}\n";
}

// ---------------------------------------------------------------------------
//  Minimal JSON reader : handles only the format written above.
//  Lines of the form:   "key": 1234567890
// ---------------------------------------------------------------------------
PatchCache patch_cache_load(const std::string& cache_path)
{
    PatchCache cache;

    std::ifstream f(cache_path);
    if (!f) return cache; // missing file : empty cache, not an error

    std::string line;
    while (std::getline(f, line)) {
        auto q1 = line.find('"');
        if (q1 == std::string::npos) continue;
        auto q2 = line.find('"', q1 + 1);
        if (q2 == std::string::npos) continue;

        std::string key = line.substr(q1 + 1, q2 - q1 - 1);
        if (key.empty()) continue;

        auto colon = line.find(':', q2 + 1);
        if (colon == std::string::npos) continue;

        std::string val_str = line.substr(colon + 1);
        size_t vs = val_str.find_first_of("0123456789");
        if (vs == std::string::npos) continue;
        size_t ve = val_str.find_last_of("0123456789");
        val_str = val_str.substr(vs, ve - vs + 1);

        try {
            uint32_t crc = static_cast<uint32_t>(std::stoul(val_str));
            cache[key] = crc;
        } catch (...) {
            throw std::runtime_error("patch_cache_load: bad value for key \"" + key + "\"");
        }
    }

    return cache;
}

// ---------------------------------------------------------------------------
//  effective_rel_path : canonical install-relative path for a manifest entry.
//
//  Most entries have an empty tar_name; their src_name carries a leading
//  platform segment ("Windows/") that must be stripped so files land under
//  the game root, not in a "Windows" sub-directory.
//  When tar_name is explicitly set (Bin/ files etc.) it is always used as-is.
// ---------------------------------------------------------------------------
static std::string effective_rel_path(const PatchFile& pf)
{
    if (!pf.tar_name.empty())
        return pf.tar_name;

    static const std::string win_prefix = "Windows/";
    if (pf.src_name.size() > win_prefix.size() &&
        pf.src_name.compare(0, win_prefix.size(), win_prefix) == 0)
        return pf.src_name.substr(win_prefix.size());

    return pf.src_name;
}

// ---------------------------------------------------------------------------

std::vector<PatchFile> patch_filter_needed(const std::vector<PatchFile>& all,
                                           const PatchCache& cache)
{
    std::vector<PatchFile> needed;
    for (const auto& pf : all) {
        // LoginMessages.xml is cached under a fixed AppData key, not game_root.
        if (pf.src_name.find("LoginMessages.xml") != std::string::npos
                && pf.src_name.find("PatchClient") != std::string::npos)
            continue;

        std::string key = effective_rel_path(pf);
        for (auto& c : key) if (c == '\\') c = '/';

        auto it = cache.find(key);
        if (it == cache.end() || it->second != pf.crc)
            needed.push_back(pf);
    }
    return needed;
}

// ---------------------------------------------------------------------------

std::optional<PatchFile> patch_find_login_messages(const std::vector<PatchFile>& all)
{
    for (const auto& pf : all) {
        if (pf.src_name.empty())
            continue;
        if (pf.src_name.find("PatchClient") == std::string::npos)
            continue;
        if (pf.src_name.find("LoginMessages.xml") == std::string::npos)
            continue;

        LOG_STAT("PATCH_CLIENT", "LoginMessages.xml manifest entry: " + pf.src_name);
        return pf;
    }
    return std::nullopt;
}

bool patch_update_login_messages(const PatchInfo&    info,
                                 const PatchFile&    pf,
                                 const std::string&  dest_path,
                                 const std::string&  cache_path,
                                 PatchFileProgressFn on_progress)
{
    static const std::string cache_key = "LoginMessages.xml";

    PatchCache cache = patch_cache_load(cache_path);
    auto it = cache.find(cache_key);
    if (it != cache.end() && it->second == pf.crc) {
        LOG_STAT("PATCH_CLIENT", "LoginMessages.xml up to date.");
        return false;
    }

    std::string url = info.url_prefix;
    if (!url.empty() && url.back() != '/' && !pf.src_name.empty() && pf.src_name.front() != '/')
        url += '/';
    url += pf.src_name + info.url_suffix;

    make_parent_dirs(dest_path);
    LOG_STAT("PATCH_CLIENT", "Downloading LoginMessages.xml from CDN.");

    if (!patch_download_file(url, dest_path, "LoginMessages.xml", on_progress))
        throw std::runtime_error("patch_update_login_messages: CDN returned 403/404 for LoginMessages.xml");

    cache[cache_key] = pf.crc;
    patch_cache_save(cache_path, cache);
    LOG_STAT("PATCH_CLIENT", "LoginMessages.xml saved to " + dest_path);
    return true;
}

// ---------------------------------------------------------------------------
//  patch_update : download needed files, persist cache after each one.
// ---------------------------------------------------------------------------
void patch_update(const PatchInfo&              info,
                  const std::vector<PatchFile>&  files,
                  const std::string&             game_root,
                  const std::string&             cache_path,
                  const PatchCallbacks&          callbacks)
{
    PatchCache cache = patch_cache_load(cache_path);

    // One session + connection for the entire run : one DNS lookup, reused TCP.
    PatchHttpClient client(info.url_prefix.empty() ? info.list_file_url : info.url_prefix);

    int total     = static_cast<int>(files.size());
    int completed = 0;

    for (const auto& pf : files) {
        // Build CDN URL: ensure exactly one '/' between prefix and src_name.
        std::string url = info.url_prefix;
        if (!url.empty() && url.back() != '/' && !pf.src_name.empty() && pf.src_name.front() != '/')
            url += '/';
        url += pf.src_name + info.url_suffix;

        // Derive the canonical install-relative path and the full on-disk dest.
        std::string rel = effective_rel_path(pf);
        std::string dest = game_root;
        if (!dest.empty() && dest.back() != '\\' && dest.back() != '/')
            dest += '\\';
        std::string rel_backslash = rel;
        for (auto& c : rel_backslash) if (c == '/') c = '\\';
        dest += rel_backslash;

        make_parent_dirs(dest);

        // Retry up to 3 times; on a retriable failure reconnect (new TCP, same
        // session/DNS) before the next attempt.
        const int MAX_ATTEMPTS = 3;
        std::string last_err;
        bool downloaded = false;
        bool gave_up    = false;

        for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
            if (attempt > 0) {
                Sleep(static_cast<DWORD>(1000 * attempt));
                try { client.reconnect(); }
                catch (const std::runtime_error& e) {
                    last_err = e.what();
                    LOG_WARN("PATCH_CLIENT", "reconnect failed: " + last_err);
                    continue;
                }
            }

            auto result = client.get(url, dest, rel, callbacks.on_file_progress, last_err);

            if (result == GetResult::Downloaded) { downloaded = true; break; }
            if (result == GetResult::Skipped) {
                // Fire file progress at 100% so the bar reflects the skip visually.
                if (callbacks.on_file_progress) callbacks.on_file_progress(rel, 100);
                break;
            }

            // Retriable : log and try again.
            LOG_WARN("PATCH_CLIENT", "attempt " + std::to_string(attempt + 1)
                                     + " failed: " + last_err);
            if (attempt == MAX_ATTEMPTS - 1)
                gave_up = true;
        }

        if (gave_up)
            throw std::runtime_error("patch_update: " + last_err);

        if (downloaded) {
            std::string key = rel;
            for (auto& c : key) if (c == '\\') c = '/';
            cache[key] = pf.crc;
            patch_cache_save(cache_path, cache);
        }

        ++completed;
        if (callbacks.on_overall_progress) {
            int pct = (total > 0) ? (completed * 100 / total) : 100;
            callbacks.on_overall_progress(pct);
        }
    }
}
