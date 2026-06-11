#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
//  Private JSON helpers for wizlauncher disk files and WebView2 messages.
// ---------------------------------------------------------------------------

using JsonU32Map = std::unordered_map<std::string, uint32_t>;

struct JsonAccountEntry {
    std::string username;
    std::string password;
};

// Flat JSON object (UTF-8 string, as read from disk).
std::string json_get(const std::string& json, const std::string& key);
bool json_get_bool(const std::string& json, const std::string& key,
                   bool default_val = false);
std::string json_escape(const std::string& s);

// Flat JSON object (UTF-16 string, WebView2 postMessage).
std::wstring json_get_wstring(const std::wstring& json, const std::wstring& key);
bool json_get_wbool(const std::wstring& json, const std::wstring& key,
                    bool default_val = false);

// File I/O helpers.
void json_ensure_parent_dir(const std::string& path);
std::string json_read_text_file(const std::string& path);
bool json_write_text_file(const std::string& path, const std::string& content);

// accounts.json : { "accounts": [ { "username", "password" }, ... ] }
std::vector<JsonAccountEntry> json_parse_accounts(const std::string& json);
std::string json_serialize_accounts(const std::vector<JsonAccountEntry>& accounts);

// patch_cache.json : flat { "tar/name.wad": <crc_u32>, ... }
JsonU32Map json_parse_u32_map(const std::string& json);
std::string json_serialize_u32_map(const JsonU32Map& map);
