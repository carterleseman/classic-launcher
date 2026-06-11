#include "json.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <fstream>
#include <sstream>
#include <stdexcept>

// ---------------------------------------------------------------------------
//  Shared UTF-8 helpers
// ---------------------------------------------------------------------------

static size_t json_skip_ws(const std::string& s, size_t pos)
{
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' ||
                               s[pos] == '\r' || s[pos] == '\n'))
        ++pos;
    return pos;
}

static size_t json_find_value(const std::string& json, const std::string& key)
{
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return std::string::npos;
    pos = json_skip_ws(json, pos + needle.size());
    if (pos >= json.size() || json[pos] != ':') return std::string::npos;
    return json_skip_ws(json, pos + 1);
}

static std::string json_read_string_at(const std::string& json, size_t& pos)
{
    pos = json_skip_ws(json, pos);
    if (pos >= json.size() || json[pos] != '"')
        return {};
    ++pos;
    std::string val;
    for (; pos < json.size(); ++pos) {
        if (json[pos] == '\\' && pos + 1 < json.size()) { val += json[++pos]; continue; }
        if (json[pos] == '"') { ++pos; break; }
        val += json[pos];
    }
    return val;
}

static size_t json_find_array(const std::string& json, const std::string& key)
{
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return std::string::npos;
    pos = json_skip_ws(json, pos + needle.size());
    if (pos >= json.size() || json[pos] != ':') return std::string::npos;
    pos = json_skip_ws(json, pos + 1);
    if (pos >= json.size() || json[pos] != '[') return std::string::npos;
    return pos + 1;
}

// ---------------------------------------------------------------------------
//  Flat object (UTF-8)
// ---------------------------------------------------------------------------

std::string json_get(const std::string& json, const std::string& key)
{
    size_t pos = json_find_value(json, key);
    if (pos == std::string::npos || pos >= json.size() || json[pos] != '"')
        return {};
    ++pos;
    std::string val;
    for (; pos < json.size(); ++pos) {
        if (json[pos] == '\\' && pos + 1 < json.size()) { val += json[++pos]; continue; }
        if (json[pos] == '"') break;
        val += json[pos];
    }
    return val;
}

bool json_get_bool(const std::string& json, const std::string& key, bool default_val)
{
    size_t pos = json_find_value(json, key);
    if (pos == std::string::npos) return default_val;
    if (pos + 4 <= json.size() && json.substr(pos, 4) == "true")  return true;
    if (pos + 5 <= json.size() && json.substr(pos, 5) == "false") return false;
    return default_val;
}

std::string json_escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\') { out += "\\\\"; continue; }
        if (c == '"')  { out += "\\\""; continue; }
        out += c;
    }
    return out;
}

// ---------------------------------------------------------------------------
//  Flat object (UTF-16, WebView2 postMessage)
// ---------------------------------------------------------------------------

std::wstring json_get_wstring(const std::wstring& json, const std::wstring& key)
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

bool json_get_wbool(const std::wstring& json, const std::wstring& key, bool default_val)
{
    std::wstring needle = L"\"" + key + L"\":";
    auto pos = json.find(needle);
    if (pos == std::wstring::npos) return default_val;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == L' ' || json[pos] == L'\t')) ++pos;
    if (pos + 4 <= json.size() && json.substr(pos, 4) == L"true")  return true;
    if (pos + 5 <= json.size() && json.substr(pos, 5) == L"false") return false;
    return default_val;
}

// ---------------------------------------------------------------------------
//  File I/O
// ---------------------------------------------------------------------------

void json_ensure_parent_dir(const std::string& path)
{
    auto slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return;
    std::string dir = path.substr(0, slash);
    for (size_t i = 0; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '/' || dir[i] == '\\') {
            std::string partial(dir.begin(), dir.begin() + i);
            if (!partial.empty())
                CreateDirectoryA(partial.c_str(), nullptr);
        }
    }
}

std::string json_read_text_file(const std::string& path)
{
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool json_write_text_file(const std::string& path, const std::string& content)
{
    json_ensure_parent_dir(path);
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << content;
    return true;
}

// ---------------------------------------------------------------------------
//  accounts.json
// ---------------------------------------------------------------------------

std::vector<JsonAccountEntry> json_parse_accounts(const std::string& json)
{
    std::vector<JsonAccountEntry> result;

    size_t pos = json_find_array(json, "accounts");
    if (pos == std::string::npos) return result;

    pos = json_skip_ws(json, pos);
    while (pos < json.size() && json[pos] != ']') {
        if (json[pos] == ',') { ++pos; pos = json_skip_ws(json, pos); continue; }
        if (json[pos] != '{') break;

        ++pos;
        JsonAccountEntry entry;
        while (pos < json.size() && json[pos] != '}') {
            pos = json_skip_ws(json, pos);
            if (pos < json.size() && json[pos] == ',') { ++pos; continue; }

            std::string field = json_read_string_at(json, pos);
            pos = json_skip_ws(json, pos);
            if (pos >= json.size() || json[pos] != ':') break;
            ++pos;

            std::string value = json_read_string_at(json, pos);
            if (field == "username") entry.username = value;
            else if (field == "password") entry.password = value;
        }

        if (!entry.username.empty())
            result.push_back(std::move(entry));

        if (pos < json.size() && json[pos] == '}') ++pos;
        pos = json_skip_ws(json, pos);
    }

    return result;
}

std::string json_serialize_accounts(const std::vector<JsonAccountEntry>& accounts)
{
    std::ostringstream f;
    f << "{\n  \"accounts\": [\n";
    for (size_t i = 0; i < accounts.size(); ++i) {
        if (i > 0) f << ",\n";
        f << "    {\n"
          << "      \"username\": \"" << json_escape(accounts[i].username) << "\",\n"
          << "      \"password\": \"" << json_escape(accounts[i].password) << "\"\n"
          << "    }";
    }
    f << "\n  ]\n}\n";
    return f.str();
}

// ---------------------------------------------------------------------------
//  patch_cache.json
// ---------------------------------------------------------------------------

JsonU32Map json_parse_u32_map(const std::string& json)
{
    JsonU32Map cache;

    std::istringstream stream(json);
    std::string line;
    while (std::getline(stream, line)) {
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
            throw std::runtime_error("json_parse_u32_map: bad value for key \"" + key + "\"");
        }
    }

    return cache;
}

std::string json_serialize_u32_map(const JsonU32Map& map)
{
    std::ostringstream f;
    f << "{\n";
    bool first = true;
    for (const auto& kv : map) {
        if (!first) f << ",\n";
        f << "  \"" << json_escape(kv.first) << "\": " << kv.second;
        first = false;
    }
    f << "\n}\n";
    return f.str();
}
