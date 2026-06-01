#define WIN32_LEAN_AND_MEAN
#include "logger.h"

#include <windows.h>

#include <fstream>
#include <mutex>
#include <string>
#include <cstdio>

static std::mutex  g_log_mutex;
static std::string g_log_path;

// ---------------------------------------------------------------------------
void log_init()
{
    char appdata[MAX_PATH]{};
    if (!GetEnvironmentVariableA("APPDATA", appdata, sizeof(appdata))) return;

    std::string dir = std::string(appdata) + "\\wizlauncher";

    CreateDirectoryA(dir.c_str(), nullptr); // ERROR_ALREADY_EXISTS is fine

    g_log_path = dir + "\\wizlauncher.log";

    // Truncate : each run starts with a fresh log.
    std::ofstream(g_log_path, std::ios::trunc);
}

// ---------------------------------------------------------------------------
void log_write(const char* level,
               const char* tag,
               const char* file,
               int         line,
               const std::string& msg)
{
    if (g_log_path.empty()) return;

    SYSTEMTIME t;
    GetLocalTime(&t);

    // MM/DD/YY HH:MM:SS [LEVEL] TAG file.cpp NNNN message
    char header[256];
    std::snprintf(header, sizeof(header),
        "%02d/%02d/%02d %02d:%02d:%02d [%s] %s %s %04d ",
        t.wMonth, t.wDay, t.wYear % 100,
        t.wHour,  t.wMinute, t.wSecond,
        level, tag, file, line);

    std::string line_str = std::string(header) + msg + "\n";

    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        std::ofstream f(g_log_path, std::ios::app);
        f << line_str;
    }

    // Mirror to the debug output window.
    OutputDebugStringA(line_str.c_str());
}
