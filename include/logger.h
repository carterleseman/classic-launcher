#pragma once

#include <string>

// ---------------------------------------------------------------------------
//  Logger : writes to %APPDATA%\wizlauncher\wizlauncher.log
//
//  Format:
//    MM/DD/YY HH:MM:SS [LEVEL] TAG file.cpp NNNN message
//
//  Levels:
//    STAT : general status / informational
//    WARN : non-fatal oddity (skipped file, optional param missing, etc.)
//    ERR  : failure that affects functionality
// ---------------------------------------------------------------------------

// Call once at startup (before any background threads) to truncate the log
// from the previous run.
void log_init();

void log_write(const char* level,
               const char* tag,
               const char* file,
               int         line,
               const std::string& msg);

// ---------------------------------------------------------------------------
//  Runtime filename extraction : strips directory prefix from __FILE__.
// ---------------------------------------------------------------------------
namespace log_detail {
    inline const char* base_name(const char* p) {
        const char* b = p;
        for (; *p; ++p)
            if (*p == '\\' || *p == '/') b = p + 1;
        return b;
    }
}
#define _LOG_FILE log_detail::base_name(__FILE__)

#define LOG_STAT(tag, msg) log_write("STAT", (tag), _LOG_FILE, __LINE__, (msg))
#define LOG_WARN(tag, msg) log_write("WARN", (tag), _LOG_FILE, __LINE__, (msg))
#define LOG_ERR(tag,  msg) log_write("ERR ", (tag), _LOG_FILE, __LINE__, (msg))
