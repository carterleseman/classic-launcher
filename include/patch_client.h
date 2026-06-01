#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

// ---------------------------------------------------------------------------
//  Metadata returned by the patch server.
// ---------------------------------------------------------------------------
struct PatchInfo {
    uint32_t    latest_version;   // Server's current patch version number.
    std::string list_file_url;    // Full URL to download LatestFileList.bin.
    std::string url_prefix;       // CDN base URL for individual game files.
    std::string url_suffix;       // CDN URL suffix (usually empty).
    uint32_t    list_file_size;   // Expected byte count of LatestFileList.bin.
    uint32_t    list_file_crc;    // CRC of LatestFileList.bin.
};

// ---------------------------------------------------------------------------
//  One entry from the parsed LatestFileList.bin.
// ---------------------------------------------------------------------------
struct PatchFile {
    std::string src_name;         // Source path on CDN (relative to URLPrefix).
    std::string tar_name;         // Target path on disk (relative to game root).
    uint32_t    file_type;
    uint32_t    size;
    uint32_t    header_size;
    uint32_t    compressed_size;
    uint32_t    crc;
    uint32_t    header_crc;
};

// ---------------------------------------------------------------------------
//  Progress callbacks : both deliver 0-100 integer percentages.
//
//  on_file_progress : called repeatedly while a single file is downloading.
//    name    : tar_name of the file (e.g. "Data/GameData/WizardCity-WC_Hub.wad")
//    percent : 0-100 completion for this file
//              (always fires at least 0 on start and 100 on finish)
//
//  on_overall_progress : called after each file finishes.
//    percent : 0-100 completion across all files in this session
// ---------------------------------------------------------------------------
using PatchFileProgressFn    = std::function<void(const std::string& name, int percent)>;
using PatchOverallProgressFn = std::function<void(int percent)>;

struct PatchCallbacks {
    PatchFileProgressFn    on_file_progress    = nullptr; // individual file: 0-100
    PatchOverallProgressFn on_overall_progress = nullptr; // overall session: 0-100
};

// A simple map of tar_name -> CRC representing the local state of the install.
using PatchCache = std::unordered_map<std::string, uint32_t>;

// ---------------------------------------------------------------------------
//  Query the patch server for the latest file-list metadata.
//
//  Protocol (TCP, port 12500):
//    1. Receive SESSION_OFFER  (control opcode 0)
//    2. Send    MSG_LATEST_FILE_LIST_V2 (svc=8, msg=2)
//    3. Receive MSG_LATEST_FILE_LIST_V2 response
// ---------------------------------------------------------------------------
PatchInfo patch_query(const char* host = "patch.us.wizard101.com",
                      uint16_t    port = 12500);

// ---------------------------------------------------------------------------
//  Download a URL to a local path via WinHTTP (HTTPS capable).
//  Creates parent directories as needed. Always overwrites if present.
//  on_progress is called after each chunk with cumulative bytes and total.
//  Returns true if the file was downloaded, false if the server returned
//  403 or 404 (soft-skip : the file is restricted or absent on this CDN).
// ---------------------------------------------------------------------------
bool patch_download_file(const std::string&      url,
                         const std::string&      local_path,
                         const std::string&      display_name = {},
                         PatchFileProgressFn     on_progress  = nullptr);

//  Download a fresh LatestFileList.bin from the CDN and parse it.
std::vector<PatchFile> patch_get_file_list(const PatchInfo& info,
                                           const std::string& local_path);

// ---------------------------------------------------------------------------
//  JSON cache : stored at %APPDATA%\wizlauncher\patch_cache.json.
//
//  Format: a flat JSON object mapping tar_name (string) to CRC (number).
//  { "Data/GameData/WizardCity-WC_Hub.wad": 3285550172, ... }
//
//  The cache is the sole record of what wizlauncher has downloaded.
//  It is never written to the game's own PatchInfo folder.
// ---------------------------------------------------------------------------

std::string patch_default_cache_path();

PatchCache patch_cache_load(const std::string& cache_path);

void patch_cache_save(const std::string& cache_path, const PatchCache& cache);

// ---------------------------------------------------------------------------
//  Returns the subset of `all` whose cached CRC is missing or different from
//  the server CRC : i.e. the files that actually need downloading.
// ---------------------------------------------------------------------------
std::vector<PatchFile> patch_filter_needed(const std::vector<PatchFile>& all,
                                           const PatchCache& cache);

// ---------------------------------------------------------------------------
//  High-level update: downloads every file in `files`, updates the cache
//  after each one (so progress survives an interrupted run), and fires the
//  callbacks during and after each download.
// ---------------------------------------------------------------------------
void patch_update(const PatchInfo&              info,
                  const std::vector<PatchFile>&  files,
                  const std::string&             game_root,
                  const std::string&             cache_path,
                  const PatchCallbacks&          callbacks = {});

// ---------------------------------------------------------------------------
//  LoginMessages.xml : fetched from PatchClient on the CDN
// ---------------------------------------------------------------------------

std::optional<PatchFile> patch_find_login_messages(const std::vector<PatchFile>& all);

// Downloads LoginMessages.xml when the cached CRC differs.  Cache key is the
// fixed string "LoginMessages.xml".  Returns true when a fresh copy was written.
bool patch_update_login_messages(const PatchInfo&         info,
                                 const PatchFile&         pf,
                                 const std::string&       dest_path,
                                 const std::string&       cache_path,
                                 PatchFileProgressFn      on_progress = nullptr);
