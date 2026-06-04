#include "config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

// ---------------------------------------------------------------------------
//  config_path
// ---------------------------------------------------------------------------
std::string config_path()
{
    char appdata[MAX_PATH]{};
    if (!GetEnvironmentVariableA("APPDATA", appdata, sizeof(appdata)))
        return {};
    return std::string(appdata) + "\\wizlauncher\\config.json";
}

std::string login_messages_path()
{
    char appdata[MAX_PATH]{};
    if (!GetEnvironmentVariableA("APPDATA", appdata, sizeof(appdata)))
        return {};
    return std::string(appdata) + "\\wizlauncher\\LoginMessages.xml";
}

// ---------------------------------------------------------------------------
//  Registry helpers
// ---------------------------------------------------------------------------

static std::string reg_read_sz(HKEY key, const char* name)
{
    char  buf[MAX_PATH]{};
    DWORD len  = sizeof(buf);
    DWORD type = 0;
    if (RegQueryValueExA(key, name, nullptr, &type,
                         reinterpret_cast<LPBYTE>(buf), &len) == ERROR_SUCCESS
        && (type == REG_SZ || type == REG_EXPAND_SZ))
        return buf;
    return {};
}

static std::string reg_find_install_dir_in(HKEY root, const char* uninstall_path)
{
    HKEY uninstall;
    if (RegOpenKeyExA(root, uninstall_path, 0, KEY_READ, &uninstall) != ERROR_SUCCESS)
        return {};

    std::string result;
    char subkey_name[256]{};

    for (DWORD i = 0; ; ++i) {
        DWORD name_len = sizeof(subkey_name);
        LONG  rc = RegEnumKeyExA(uninstall, i, subkey_name, &name_len,
                                 nullptr, nullptr, nullptr, nullptr);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS)       continue;

        HKEY sub;
        if (RegOpenKeyExA(uninstall, subkey_name, 0, KEY_READ, &sub) != ERROR_SUCCESS)
            continue;

        std::string display = reg_read_sz(sub, "DisplayName");
        if (display == "Wizard101") {
            result = reg_read_sz(sub, "InstallLocation");
            if (!result.empty() && result.back() == '\\')
                result.pop_back();
            RegCloseKey(sub);
            break;
        }
        RegCloseKey(sub);
    }

    RegCloseKey(uninstall);
    return result;
}

static std::string reg_find_install_dir()
{
    static constexpr const char kUninstall[] =
        "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

    std::string result = reg_find_install_dir_in(HKEY_CURRENT_USER, kUninstall);
    if (!result.empty())
        return result;

    return reg_find_install_dir_in(
        HKEY_LOCAL_MACHINE,
        "Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall");
}

static std::string reg_read_uuid()
{
    HKEY key;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\KingsIsle",
                      0, KEY_READ, &key) != ERROR_SUCCESS)
        return {};
    std::string val = reg_read_sz(key, "UUID");
    RegCloseKey(key);
    return val;
}

// ---------------------------------------------------------------------------
//  SMBIOS-based HWID
//
//    1. GetSystemFirmwareTable(0x52534D42 /*'RSMB'*/, ...) : raw SMBIOS table.
//    2. Walk every SMBIOS structure:
//       - Type 0x02 (Baseboard, length > 7):
//           Read string indices at bytes [4],[5],[6],[7].  Keep only alnum chars
//           from each resolved string; join with '_'.  Gate: byte[7] must resolve
//           to a non-empty alnum string.  Entry: "MOBO:<joined>".
//       - Type 0x11 (Memory Device, length > 0x1a):
//           Indices at bytes [0x17],[0x1a],[0x18].  Same filtering/joining.
//           Gate: byte[0x18].  Entry: "RAM:<joined>".
//    3. Sort entries alphabetically.
//    4. Join with ':'.
//    5. MD5 the resulting ASCII string.
//
//  When no entries are collected (or SMBIOS/MD5 fails), returns "HW-ID-SMBIOS"
// ---------------------------------------------------------------------------
static std::string compute_hwid_smbios()
{
    static constexpr const char kFallback[] = "HW-ID-SMBIOS";

    typedef UINT (WINAPI *PGetSystemFirmwareTable)(DWORD, DWORD, PVOID, DWORD);
    HMODULE hK32 = GetModuleHandleA("kernel32.dll");
    if (!hK32) return kFallback;
    auto pfn = reinterpret_cast<PGetSystemFirmwareTable>(
        GetProcAddress(hK32, "GetSystemFirmwareTable"));
    if (!pfn) return kFallback;

    UINT size = pfn(0x52534D42, 0, nullptr, 0);
    if (size < 8) return kFallback;

    std::vector<uint8_t> buf(size);
    if (pfn(0x52534D42, 0, buf.data(), size) != size) return kFallback;

    // Bytes 0-3: unused SMBIOS header fields.
    // Bytes 4-7: 32-bit table length.  Table data begins at byte 8.
    uint32_t table_len = 0;
    std::memcpy(&table_len, buf.data() + 4, 4);
    if (static_cast<UINT>(table_len + 8) != size) return kFallback;

    const uint8_t* const table     = buf.data() + 8;
    const uint8_t* const table_end = table + table_len;

    // -----------------------------------------------------------------------
    //  Walk SMBIOS structures and collect "MOBO:" / "RAM:" entries.
    // -----------------------------------------------------------------------

    // Parse the null-terminated string set that follows a structure's
    // formatted area.  Returns 1-based indexed strings matching SMBIOS spec.
    auto parse_strings = [](const uint8_t* str_area,
                             const uint8_t* end) -> std::vector<std::string>
    {
        std::vector<std::string> strings;
        const uint8_t* p = str_area;
        while (p < end && *p != 0) {
            const uint8_t* s = p;
            while (p < end && *p != 0) ++p;
            strings.emplace_back(reinterpret_cast<const char*>(s), p - s);
            ++p;
        }
        return strings;
    };

    // Advance past the string set (double-null terminated) to find the
    // start of the next SMBIOS structure.
    auto next_struct = [](const uint8_t* str_area,
                          const uint8_t* end) -> const uint8_t*
    {
        const uint8_t* p = str_area;
        if (p >= end) return end;
        if (*p == 0) return p + 2 <= end ? p + 2 : end; // no strings
        while (p + 1 < end && !(*p == 0 && *(p + 1) == 0)) ++p;
        return p + 2 <= end ? p + 2 : end;
    };

    // Keep only alnum chars from a raw SMBIOS string.
    auto filter_alnum = [](const std::string& s) -> std::string {
        std::string out;
        for (unsigned char c : s)
            if (isalnum(c)) out += static_cast<char>(c);
        return out;
    };

    // Look up a 1-based string index and return its filtered form.
    auto get_filtered = [&](const std::vector<std::string>& strings,
                             uint8_t index) -> std::string {
        if (index == 0 || index > static_cast<uint8_t>(strings.size()))
            return {};
        return filter_alnum(strings[index - 1]);
    };

    // Append a non-empty filtered field to acc, inserting '_' if needed.
    auto append_field = [](std::string& acc, const std::string& field) {
        if (field.empty()) return;
        if (!acc.empty()) acc += '_';
        acc += field;
    };

    std::vector<std::string> entries;

    for (const uint8_t* p = table; p + 4 <= table_end; ) {
        const uint8_t type   = p[0];
        const uint8_t length = p[1];

        if (length < 4 || p + length > table_end) break;

        const uint8_t* str_area = p + length;
        const uint8_t* next     = next_struct(str_area, table_end);

        if (type == 0x02 && length > 7) {
            auto strings = parse_strings(str_area, next);
            std::string gate = get_filtered(strings, p[7]);
            if (!gate.empty()) {
                std::string acc;
                append_field(acc, get_filtered(strings, p[4]));
                append_field(acc, get_filtered(strings, p[5]));
                append_field(acc, get_filtered(strings, p[6]));
                append_field(acc, gate);
                entries.push_back("MOBO:" + acc);
            }
        } else if (type == 0x11 && length > 0x1a) {
            auto strings = parse_strings(str_area, next);
            std::string gate = get_filtered(strings, p[0x18]);
            if (!gate.empty()) {
                std::string acc;
                append_field(acc, get_filtered(strings, p[0x17]));
                append_field(acc, get_filtered(strings, p[0x1a]));
                append_field(acc, gate);
                entries.push_back("RAM:" + acc);
            }
        }

        p = next;
    }

    if (entries.empty()) return kFallback;

    std::sort(entries.begin(), entries.end());
    std::string combined;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) combined += ':';
        combined += entries[i];
    }

    // MD5 via Windows CNG.
    BCRYPT_ALG_HANDLE  hAlg  = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    uint8_t digest[16]{};
    bool ok = false;

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_MD5_ALGORITHM,
                                    nullptr, 0) == 0) {
        if (BCryptCreateHash(hAlg, &hHash, nullptr, 0,
                             nullptr, 0, 0) == 0) {
            (void)BCryptHashData(hHash,
                                 reinterpret_cast<PUCHAR>(combined.data()),
                                 static_cast<ULONG>(combined.size()), 0);
            ok = (BCryptFinishHash(hHash, digest, 16, 0) == 0);
            BCryptDestroyHash(hHash);
        }
        BCryptCloseAlgorithmProvider(hAlg, 0);
    }

    if (!ok) return kFallback;

    char hex[33]{};
    for (int i = 0; i < 16; ++i)
        snprintf(hex + i * 2, 3, "%02X", digest[i]);
    return hex;
}

// ---------------------------------------------------------------------------
//  JSON helpers
// ---------------------------------------------------------------------------

static size_t skip_ws(const std::string& s, size_t pos)
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
    pos = skip_ws(json, pos + needle.size());
    if (pos >= json.size() || json[pos] != ':') return std::string::npos;
    return skip_ws(json, pos + 1);
}

static std::string json_get(const std::string& json, const std::string& key)
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

static bool json_get_bool(const std::string& json, const std::string& key,
                          bool default_val = false)
{
    size_t pos = json_find_value(json, key);
    if (pos == std::string::npos) return default_val;
    if (pos + 4 <= json.size() && json.substr(pos, 4) == "true")  return true;
    if (pos + 5 <= json.size() && json.substr(pos, 5) == "false") return false;
    return default_val;
}

static std::string json_escape(const std::string& s)
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

static void ensure_dir(const std::string& path)
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

void config_save(const AppConfig& cfg)
{
    std::string path = config_path();
    ensure_dir(path);
    std::ofstream f(path, std::ios::trunc);
    if (!f) return;
    f << "{\n"
      << "  \"install_dir\": \"" << json_escape(cfg.install_dir) << "\",\n"
      << "  \"uuid\": \""        << json_escape(cfg.uuid)        << "\",\n"
      << "  \"hwid\": \""        << json_escape(cfg.hwid)        << "\"";

    if (!cfg.fixed_window_size)
        f << ",\n  \"fixed_window_size\": false";

    if (cfg.quick_launch)
        f << ",\n  \"quick_launch\": true";

    if (cfg.use_orig_auth)
        f << ",\n  \"use_orig_auth\": true";

    if (!cfg.starting_page.empty())
        f << ",\n  \"starting_page\": \"" << json_escape(cfg.starting_page) << "\"";

    if (!cfg.remembered_username.empty())
        f << ",\n  \"remembered_username\": \"" << json_escape(cfg.remembered_username) << "\"";

    if (cfg.remember_password)
        f << ",\n  \"remember_password\": true";

    if (!cfg.remembered_password.empty())
        f << ",\n  \"remembered_password\": \"" << json_escape(cfg.remembered_password) << "\"";

    f << "\n}\n";
}

AppConfig config_load()
{
    AppConfig cfg;
    bool dirty = false;

    std::string path = config_path();
    {
        std::ifstream f(path);
        if (f) {
            std::ostringstream ss;
            ss << f.rdbuf();
            std::string json = ss.str();
            cfg.install_dir         = json_get(json, "install_dir");
            cfg.uuid                = json_get(json, "uuid");
            cfg.hwid                = json_get(json, "hwid");
            cfg.fixed_window_size   = json_get_bool(json, "fixed_window_size", true);
            cfg.quick_launch        = json_get_bool(json, "quick_launch");
            cfg.use_orig_auth       = json_get_bool(json, "use_orig_auth");
            cfg.starting_page       = json_get(json, "starting_page");
            cfg.remembered_username = json_get(json, "remembered_username");
            cfg.remember_password   = json_get_bool(json, "remember_password");
            cfg.remembered_password = json_get(json, "remembered_password");
        }
    }

    if (cfg.install_dir.empty()) {
        cfg.install_dir = reg_find_install_dir();
        if (!cfg.install_dir.empty()) dirty = true;
    }

    if (cfg.uuid.empty()) {
        cfg.uuid = reg_read_uuid();
        if (!cfg.uuid.empty()) dirty = true;
    }
    if (cfg.hwid.empty()) {
        cfg.hwid = compute_hwid_smbios();
        dirty = true;
    }

    if (dirty)
        config_save(cfg);

    return cfg;
}
