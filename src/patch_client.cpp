#include "patch_client.h"
#include "dml.h"
#include "logger.h"
#include "json.h"

#include <unordered_map>
#include <string>
#include <tuple>

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

static bool ends_with_ci(const std::string& s, const char* ext)
{
    const size_t n = std::strlen(ext);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char a = static_cast<unsigned char>(s[s.size() - n + i]);
        const unsigned char b = static_cast<unsigned char>(ext[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

static uint64_t file_size_or_zero(const std::string& path)
{
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &info))
        return 0;
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return 0;
    return (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
}

static bool write_at(const std::string& path, uint64_t off, const uint8_t* p, size_t n)
{
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!f) {
        std::ofstream create(path, std::ios::binary | std::ios::app);
        create.close();
        f.open(path, std::ios::binary | std::ios::in | std::ios::out);
    }
    if (!f) return false;
    f.seekp(static_cast<std::streamoff>(off));
    f.write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(n));
    return static_cast<bool>(f);
}

struct WadEntry { uint32_t offset; uint32_t zip_size; uint32_t crc; };

static bool parse_wad_entries(const std::string& wad_path, std::vector<WadEntry>& out)
{
    std::ifstream f(wad_path, std::ios::binary);
    if (!f) return false;
    char magic[5]{};
    f.read(magic, 5);
    if (!f || std::memcmp(magic, "KIWAD", 5) != 0) return false;
    uint32_t version = 0, num_files = 0;
    f.read(reinterpret_cast<char*>(&version), 4);
    f.read(reinterpret_cast<char*>(&num_files), 4);
    if (!f) return false;
    if (version >= 2) {
        uint8_t padding = 0;
        f.read(reinterpret_cast<char*>(&padding), 1);
        if (!f) return false;
    }
    out.clear();
    out.reserve(num_files);
    for (uint32_t i = 0; i < num_files; ++i) {
        uint32_t offset = 0, size = 0, zip_size = 0, crc = 0, name_size = 0;
        uint8_t zip = 0;
        f.read(reinterpret_cast<char*>(&offset), 4);
        f.read(reinterpret_cast<char*>(&size), 4);
        f.read(reinterpret_cast<char*>(&zip_size), 4);
        f.read(reinterpret_cast<char*>(&zip), 1);
        f.read(reinterpret_cast<char*>(&crc), 4);
        f.read(reinterpret_cast<char*>(&name_size), 4);
        if (!f) return false;
        f.seekg(name_size, std::ios::cur);
        if (!f) return false;
        out.push_back({ offset, zip == 0 ? size : zip_size, crc });
    }
    return out.size() == num_files;
}

// .utd: 16-byte records, all LE: reserved 0, offset, payload, crc.
// Only body blocks that are not already inside the on-disk prefix.
static bool write_wad_utd(const std::string& wad_path)
{
    std::vector<WadEntry> entries;
    if (!parse_wad_entries(wad_path, entries))
        return false;

    const uint64_t on_disk = file_size_or_zero(wad_path);
    std::ofstream out((wad_path + ".utd").c_str(), std::ios::binary | std::ios::trunc);
    if (!out) return false;

    const uint32_t reserved = 0;
    for (const auto& e : entries) {
        const uint64_t end = static_cast<uint64_t>(e.offset) + e.zip_size;
        if (e.zip_size == 0 || end <= on_disk)
            continue;
        out.write(reinterpret_cast<const char*>(&reserved), 4);
        out.write(reinterpret_cast<const char*>(&e.offset), 4);
        out.write(reinterpret_cast<const char*>(&e.zip_size), 4);
        out.write(reinterpret_cast<const char*>(&e.crc), 4);
    }
    return static_cast<bool>(out);
}

static bool wad_header_ok(const std::string& path, uint32_t header_size)
{
    if (file_size_or_zero(path) < header_size)
        return false;
    std::vector<WadEntry> entries;
    return parse_wad_entries(path, entries);
}

static std::string package_stem(const std::string& rel)
{
    std::string stem = rel;
    const auto slash = stem.find_last_of("/\\");
    if (slash != std::string::npos)
        stem = stem.substr(slash + 1);
    const auto dot = stem.rfind('.');
    if (dot != std::string::npos)
        stem.resize(dot);
    return stem;
}

// Package catalog at the game root. Names only; the client uses this for
// RequestPackage, not download progress.
static void ensure_local_package(const std::string& game_root, const std::string& rel)
{
    if (game_root.empty()) return;
    const std::string name = package_stem(rel);
    if (name.empty()) return;

    std::string path = game_root;
    if (!path.empty() && path.back() != '\\' && path.back() != '/')
        path += '\\';
    path += "LocalPackagesList.txt";

    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line == name)
                return;
        }
    }
    std::ofstream out(path, std::ios::app);
    if (out)
        out << name << "\n";
}

// file_type 5 = header + .utd; the client fills the body after launch.
// file_type 3 (Root, zones) stays a full GET.
static bool is_streamed_wad(const PatchFile& pf, const std::string& rel)
{
    return ends_with_ci(rel, ".wad") && pf.file_type == 5
        && pf.header_size > 0 && pf.size > pf.header_size;
}

static std::string patchinfo_crc_path(const std::string& game_root,
                                      const std::string& rel)
{
    std::string dir = game_root;
    if (!dir.empty() && dir.back() != '\\' && dir.back() != '/')
        dir += '\\';
    return dir + "PatchInfo\\CRC_" + package_stem(rel) + ".dat";
}

static bool read_patchinfo_file_crc(const std::string& path, uint32_t& out_crc)
{
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    uint32_t crc = 0;
    DWORD n = 0;
    const BOOL ok = ReadFile(h, &crc, sizeof(crc), &n, nullptr);
    CloseHandle(h);
    if (!ok || n != sizeof(crc))
        return false;
    out_crc = crc;
    return true;
}

static uint64_t unix_ms_now()
{
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{};
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (u.QuadPart - 116444736000000000ULL) / 10000ULL;
}

// 24-byte PatchInfo\CRC_<name>.dat so the client knows header vs full size.
static void write_patchinfo_crc(const std::string& game_root,
                                const PatchFile&   pf,
                                const std::string& rel)
{
    if (game_root.empty()) return;

    const std::string path = patchinfo_crc_path(game_root, rel);
    make_parent_dirs(path);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        LOG_WARN("PATCH_CLIENT", "cannot write " + path);
        return;
    }
    const uint32_t file_crc    = pf.crc;
    const uint32_t file_size   = pf.size;
    const uint32_t header_crc  = pf.header_crc;
    const uint32_t header_size = pf.header_size;
    const uint64_t ts          = unix_ms_now();
    out.write(reinterpret_cast<const char*>(&file_crc), 4);
    out.write(reinterpret_cast<const char*>(&file_size), 4);
    out.write(reinterpret_cast<const char*>(&header_crc), 4);
    out.write(reinterpret_cast<const char*>(&header_size), 4);
    out.write(reinterpret_cast<const char*>(&ts), 8);
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
    // If status_out is set, it receives the HTTP status (0 if the request
    // never got a response).
    std::vector<uint8_t> download_to_memory(const std::string& url,
                                           DWORD* status_out = nullptr)
    {
        if (status_out) *status_out = 0;
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
        if (status_out) *status_out = status;
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

    bool crack_path(const std::string& url, std::vector<wchar_t>& path_buf)
    {
        std::wstring wurl(url.begin(), url.end());
        path_buf.assign(2048, L'\0');
        URL_COMPONENTS uc{};
        uc.dwStructSize    = sizeof(uc);
        uc.lpszUrlPath     = path_buf.data();
        uc.dwUrlPathLength = static_cast<DWORD>(path_buf.size());
        return WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) != FALSE;
    }

    bool get_range(const std::string& url, uint64_t start, uint64_t end,
                   std::vector<uint8_t>& out, std::string& out_err)
    {
        out.clear();
        std::vector<wchar_t> path_buf;
        if (!crack_path(url, path_buf)) {
            out_err = "invalid URL: " + url;
            return false;
        }

        HINTERNET req = WinHttpOpenRequest(conn, L"GET", path_buf.data(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, req_flags);
        if (!req) {
            out_err = "WinHttpOpenRequest failed (err=" + std::to_string(GetLastError()) + ")";
            return false;
        }

        const std::wstring hdrs =
            L"Accept-Encoding: identity\r\nRange: bytes=" +
            std::to_wstring(start) + L"-" + std::to_wstring(end);
        WinHttpAddRequestHeaders(req, hdrs.c_str(), (DWORD)-1,
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            || !WinHttpReceiveResponse(req, nullptr)) {
            const DWORD err = GetLastError();
            WinHttpCloseHandle(req);
            req = nullptr;
            if (err == 12152 || err == 12030) {
                try { reconnect(); } catch (...) {}
                req = WinHttpOpenRequest(conn, L"GET", path_buf.data(), nullptr,
                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, req_flags);
                if (req) {
                    WinHttpAddRequestHeaders(req, hdrs.c_str(), (DWORD)-1,
                                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
                    if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                        && WinHttpReceiveResponse(req, nullptr))
                        goto range_ok;
                    WinHttpCloseHandle(req);
                    req = nullptr;
                }
            }
            out_err = "range send/receive failed (err=" + std::to_string(err) + ")";
            return false;
        }
    range_ok:
        if (!req) {
            out_err = "range send/receive failed";
            return false;
        }

        DWORD status = 0;
        {
            DWORD len = sizeof(status);
            WinHttpQueryHeaders(req,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                WINHTTP_NO_HEADER_INDEX);
        }
        if (status != 200 && status != 206) {
            out_err = "range HTTP " + std::to_string(status);
            WinHttpCloseHandle(req);
            return false;
        }

        std::vector<uint8_t> chunk(65536);
        DWORD bytes_read = 0;
        while (WinHttpReadData(req, chunk.data(), static_cast<DWORD>(chunk.size()), &bytes_read)
               && bytes_read > 0)
            out.insert(out.end(), chunk.begin(), chunk.begin() + bytes_read);
        WinHttpCloseHandle(req);
        return !out.empty();
    }

    bool write_wad_header(const std::string& hdr_gz_url, const std::string& dest,
                          std::string& out_err)
    {
        DWORD status = 0;
        auto compressed = download_to_memory(hdr_gz_url, &status);
        if (compressed.empty()) {
            out_err = "wad header download failed (HTTP " + std::to_string(status) + ")";
            return false;
        }
        auto raw = gz_decompress(compressed.data(), compressed.size());
        if (raw.empty()) {
            out_err = "wad header gz decompress failed";
            return false;
        }
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out) {
            out_err = "cannot write " + dest;
            return false;
        }
        out.write(reinterpret_cast<const char*>(raw.data()),
                  static_cast<std::streamsize>(raw.size()));
        return static_cast<bool>(out);
    }

    // Type-5: write the .hdr.gz directory as the .wad and a .utd of missing
    // body blocks.  The graphical client fills the rest after we launch it.
    GetResult get_wad_header(const std::string& url,
                             const std::string& dest,
                             const std::string& display_name,
                             uint32_t header_size,
                             PatchFileProgressFn on_progress,
                             std::string& out_err)
    {
        if (on_progress) on_progress(display_name, 0);
        if (!write_wad_header(url + ".hdr.gz", dest, out_err)) {
            if (out_err.find("HTTP 403") != std::string::npos
                || out_err.find("HTTP 404") != std::string::npos)
                return GetResult::Skipped;
            return GetResult::Retriable;
        }
        if (header_size > 0 && file_size_or_zero(dest) < header_size) {
            out_err = "wad header short: got "
                      + std::to_string(file_size_or_zero(dest))
                      + " expected " + std::to_string(header_size);
            return GetResult::Retriable;
        }
        if (!write_wad_utd(dest)) {
            out_err = "cannot write " + dest + ".utd";
            return GetResult::Retriable;
        }
        LOG_STAT("PATCH_CLIENT", "wrote " + display_name + " header ("
                 + std::to_string(file_size_or_zero(dest)) + " bytes) + .utd");
        if (on_progress) on_progress(display_name, 100);
        return GetResult::Downloaded;
    }

    // Type-5: header from .hdr.gz, then bounded Ranges from header_size.
    // A full GET of these URLs stops at a prefix and must not be used.
    GetResult download_ranged_wad(const std::string& url,
                                  const std::string& dest,
                                  const std::string& display_name,
                                  uint32_t header_size,
                                  uint32_t expected_size,
                                  PatchFileProgressFn on_progress,
                                  std::string& out_err)
    {
        DeleteFileA((dest + ".utd").c_str());

        if (file_size_or_zero(dest) < header_size) {
            if (!write_wad_header(url + ".hdr.gz", dest, out_err))
                return GetResult::Retriable;
            LOG_STAT("PATCH_CLIENT", "wrote " + display_name + " header ("
                     + std::to_string(file_size_or_zero(dest)) + " bytes)");
        }

        uint64_t off = file_size_or_zero(dest);
        if (off < header_size)
            off = header_size;

        const uint64_t CHUNK = 4ull * 1024 * 1024;

        while (off < expected_size) {
            uint64_t end = off + CHUNK - 1;
            if (end >= expected_size)
                end = expected_size - 1;

            std::vector<uint8_t> chunk;
            std::string range_err;
            if (get_range(url, off, end, chunk, range_err) && !chunk.empty()) {
                if (!write_at(dest, off, chunk.data(), chunk.size())) {
                    out_err = "write failed at offset " + std::to_string(off);
                    return GetResult::Retriable;
                }
                off += chunk.size();
                if (on_progress)
                    on_progress(display_name, http_pct(off, expected_size));
                continue;
            }

            const uint64_t probe0 = off + 64;
            const uint64_t probe1 = (probe0 + 65535 < expected_size)
                ? probe0 + 65535 : expected_size - 1;
            if (probe0 < expected_size
                && get_range(url, probe0, probe1, chunk, range_err)
                && !chunk.empty()) {
                if (write_at(dest, probe0, chunk.data(), chunk.size())) {
                    off = probe0 + chunk.size();
                    if (on_progress)
                        on_progress(display_name, http_pct(off, expected_size));
                    continue;
                }
            }

            std::vector<WadEntry> entries;
            if (parse_wad_entries(dest, entries)) {
                for (const auto& e : entries) {
                    const uint64_t e0 = e.offset;
                    const uint64_t e1 = e0 + e.zip_size;
                    if (e.zip_size == 0 || e1 <= off || e0 > end)
                        continue;
                    std::vector<uint8_t> block;
                    std::string berr;
                    if (!get_range(url, e0, e1 - 1, block, berr) || block.empty())
                        continue;
                    write_at(dest, e0, block.data(), block.size());
                }
            }

            off = end + 1;
            if (on_progress)
                on_progress(display_name, http_pct(off, expected_size));
        }

        if (file_size_or_zero(dest) < expected_size) {
            out_err = "ranged download short: got "
                      + std::to_string(file_size_or_zero(dest))
                      + " expected " + std::to_string(expected_size);
            return GetResult::Retriable;
        }
        DeleteFileA((dest + ".utd").c_str());
        if (on_progress) on_progress(display_name, 100);
        return GetResult::Downloaded;
    }

    // Download url to local_path (full GET, no Range).
    //   Downloaded : file written; if expected_size > 0, on-disk size matches.
    //   Skipped    : server returned 403 or 404.
    //   Retriable  : transport failure or short body.
    GetResult get(const std::string& url,
                  const std::string& local_path,
                  const std::string& display_name,
                  PatchFileProgressFn on_progress,
                  std::string& out_err,
                  uint64_t expected_size = 0)
    {
        std::vector<wchar_t> path_buf;
        if (!crack_path(url, path_buf)) {
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
                auto compressed = download_to_memory(gz_url);
                if (!compressed.empty()) {
                    auto decompressed = gz_decompress(compressed.data(), compressed.size());
                    if (!decompressed.empty()) {
                        std::ofstream out(local_path, std::ios::binary | std::ios::trunc);
                        if (out) {
                            out.write(reinterpret_cast<const char*>(decompressed.data()),
                                      static_cast<std::streamsize>(decompressed.size()));
                            if (on_progress) on_progress(display_name, 100);
                            return GetResult::Downloaded;
                        }
                    } else {
                        LOG_ERR("PATCH_CLIENT", "gz decompress failed for: " + gz_url);
                    }
                }
            }

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
        const uint64_t progress_total = expected_size > 0 ? expected_size : content_length;

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
            out.write(buf.data(), static_cast<std::streamsize>(bytes_read));
            if (!out) {
                out_err = "write failed for " + local_path;
                WinHttpCloseHandle(req);
                return GetResult::Retriable;
            }
            bytes_done += bytes_read;
            if (on_progress)
                on_progress(display_name, http_pct(bytes_done, progress_total));
        }

        out.close();
        WinHttpCloseHandle(req);

        const uint64_t final_size = file_size_or_zero(local_path);
        if (expected_size > 0 && final_size < expected_size) {
            out_err = "short download of " + display_name + ": got "
                      + std::to_string(final_size) + " expected "
                      + std::to_string(expected_size);
            return GetResult::Retriable;
        }
        if (bytes_done == 0 && content_length > 0 && final_size == 0) {
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

    LOG_DBGM("CORE_PATCH_SYS", "Start downloading " + local_path);

    const int MAX_ATTEMPTS = 3;
    std::string last_err;

    for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
        if (attempt > 0)
            Sleep(static_cast<DWORD>(1000 * attempt));

        try {
            PatchHttpClient client(url);
            auto result = client.get(url, local_path, display_name, on_progress, last_err);
            if (result == GetResult::Downloaded) {
                LOG_DBGM("CORE_PATCH_SYS", "Done downloading  " + local_path);
                return true;
            }
            if (result == GetResult::Skipped) {
                LOG_DBGM("CORE_PATCH_SYS", "skip " + local_path);
                return false;
            }
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

        // Skip entries with no CDN path.  PatchClient files are not part of
        // the game install we maintain.
        if (!pf.src_name.empty() &&
            pf.src_name.find("PatchClient") == std::string::npos)
            records.push_back(pf);

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
    JsonU32Map normalized;
    normalized.reserve(cache.size());
    for (const auto& kv : cache) {
        std::string key = kv.first;
        for (auto& c : key)
            if (c == '\\') c = '/';
        normalized[key] = kv.second;
    }
    if (!json_write_text_file(cache_path, json_serialize_u32_map(normalized)))
        throw std::runtime_error("patch_cache_save: cannot write " + cache_path);
}

PatchCache patch_cache_load(const std::string& cache_path)
{
    return json_parse_u32_map(json_read_text_file(cache_path));
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

static std::string join_game_path(const std::string& game_root, const std::string& rel)
{
    std::string dest = game_root;
    if (!dest.empty() && dest.back() != '\\' && dest.back() != '/')
        dest += '\\';
    std::string rel_bs = rel;
    for (auto& c : rel_bs) if (c == '/') c = '\\';
    dest += rel_bs;
    return dest;
}

// ---------------------------------------------------------------------------

std::vector<PatchFile> patch_filter_needed(const std::vector<PatchFile>& all,
                                           PatchCache& cache,
                                           const std::string& game_root,
                                           bool& cache_changed,
                                           PatchOverallProgressFn on_scan)
{
    cache_changed = false;
    std::vector<PatchFile> needed;
    needed.reserve(all.size());
    const size_t total = all.size();
    for (size_t i = 0; i < total; ++i) {
        const auto& pf = all[i];
        std::string key = effective_rel_path(pf);
        for (auto& c : key) if (c == '\\') c = '/';

        const std::string dest = game_root.empty() ? std::string()
                                                   : join_game_path(game_root, key);
        const uint64_t on_disk = dest.empty() ? 0 : file_size_or_zero(dest);

        auto it = cache.find(key);
        const bool json_match = (it != cache.end() && it->second == pf.crc);
        const bool streamed = is_streamed_wad(pf, key);

        bool disk_ok = false;
        // Type-5 bodies are finished here.  A header + .utd is not enough:
        // WizardGraphicalClient launched with -L says Already Have and
        // downloads 0 bytes (WizardClient.log, 11/01 17:59).
        if (pf.size > 0)
            disk_ok = on_disk >= pf.size;
        else
            disk_ok = json_match;

        bool up_to_date = false;
        if (disk_ok && json_match) {
            up_to_date = true;
        } else if (disk_ok) {
            uint32_t dat_crc = 0;
            const bool has_dat = !game_root.empty()
                && read_patchinfo_file_crc(patchinfo_crc_path(game_root, key), dat_crc);
            if (has_dat && dat_crc != pf.crc) {
                up_to_date = false;
            } else {
                up_to_date = true;
                cache[key] = pf.crc;
                cache_changed = true;
            }
        }

        // A leftover .utd on a complete type-3 WAD makes the client treat it
        // as incomplete.  Once JSON matches, that cleanup already happened.
        if (up_to_date && !json_match && !dest.empty()
            && ends_with_ci(key, ".wad") && !streamed)
            DeleteFileA((dest + ".utd").c_str());

        if (!up_to_date)
            needed.push_back(pf);
            
        if (on_scan && ((i & 127) == 127 || i + 1 == total)) {
            int pct = total ? static_cast<int>(((i + 1) * 100) / total) : 0;
            if (pct > 99) pct = 99;
            on_scan(pct);
        }
    }
    return needed;
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

        LOG_DBGM("CORE_PATCH_SYS", "Start downloading " + dest);

        const bool streamed = is_streamed_wad(pf, rel);

        // Complete-but-stale (CRC changed): replace.  A short type-5 file
        // is extended in place from its current size, not truncated.
        if (!streamed && pf.size > 0 && file_size_or_zero(dest) >= pf.size)
            DeleteFileA(dest.c_str());
        
        std::string last_err;
        bool downloaded = false;
        bool skipped    = false;
        bool gave_up    = false;
        int  stall      = 0;
        uint64_t last_sz = file_size_or_zero(dest);

        for (int attempt = 0; ; ++attempt) {
            if (attempt > 0) {
                Sleep(static_cast<DWORD>(1000 * (stall + 1)));
                try { client.reconnect(); }
                catch (const std::runtime_error& e) {
                    last_err = e.what();
                    LOG_WARN("PATCH_CLIENT", "reconnect failed: " + last_err);
                    if (++stall >= 3) { gave_up = true; break; }
                    continue;
                }
            }

            auto result = streamed
                ? client.download_ranged_wad(url, dest, rel, pf.header_size, pf.size,
                                             callbacks.on_file_progress, last_err)
                : client.get(url, dest, rel, callbacks.on_file_progress,
                             last_err, pf.size);

            if (result == GetResult::Downloaded) { downloaded = true; break; }
            if (result == GetResult::Skipped) {
                skipped = true;
                if (callbacks.on_file_progress) callbacks.on_file_progress(rel, 100);
                break;
            }

            LOG_WARN("PATCH_CLIENT", last_err);
            const uint64_t now = file_size_or_zero(dest);
            if (now > last_sz) { last_sz = now; stall = 0; }
            else if (++stall >= 3) { gave_up = true; break; }
        }

        if (gave_up)
            throw std::runtime_error("patch_update: " + last_err);

        if (skipped)
            LOG_DBGM("CORE_PATCH_SYS", "skip " + dest);
        else if (downloaded)
            LOG_DBGM("CORE_PATCH_SYS", "Done downloading  " + dest);

        const uint64_t got = file_size_or_zero(dest);
        const bool complete_enough = pf.size == 0 || got >= pf.size;

        if (downloaded && complete_enough) {
            std::string key = rel;
            for (auto& c : key) if (c == '\\') c = '/';
            cache[key] = pf.crc;
            patch_cache_save(cache_path, cache);
            write_patchinfo_crc(game_root, pf, rel);
            ensure_local_package(game_root, rel);

            if (!streamed && ends_with_ci(rel, ".wad"))
                DeleteFileA((dest + ".utd").c_str());
        }

        ++completed;
        if (callbacks.on_overall_progress) {
            int pct = (total > 0) ? (completed * 100 / total) : 100;
            callbacks.on_overall_progress(pct);
        }
    }
}
