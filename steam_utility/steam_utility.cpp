// steam_utility.cpp
// Lightweight Steam tracking companion for wizlauncher.
//
// Usage:  steam_utility.exe idle <app_id> <dll_dir>
//
// Loads steam_api64.dll from dll_dir, registers a Steam play session for the
// given AppID, then polls for WizardGraphicalClient.exe every 60 seconds and
// exits cleanly when no instances are found.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <string>

// SDK 1.57+: int SteamAPI_InitFlat(char pOutErrMsg[1024]) — 0 = OK
typedef int  (__cdecl *PFN_SteamAPI_InitFlat)(char* pOutErrMsg);
typedef void (__cdecl *PFN_SteamAPI_Shutdown)();
typedef void (__cdecl *PFN_SteamAPI_RunCallbacks)();

static bool is_process_running(const char* name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;

    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, name) == 0) {
                found = true;
                break;
            }
        } while (Process32Next(snap, &pe));
    }

    CloseHandle(snap);
    return found;
}

int main(int argc, char* argv[])
{
    if (argc < 4 || _stricmp(argv[1], "idle") != 0)
        return 1;

    const char* app_id_str = argv[2];
    const char* dll_dir    = argv[3];

    std::string appid_file = std::string(dll_dir) + "\\steam_appid.txt";
    {
        FILE* f = nullptr;
        if (fopen_s(&f, appid_file.c_str(), "w") == 0 && f) {
            fputs(app_id_str, f);
            fclose(f);
        }
    }

    SetEnvironmentVariableA("SteamAppId", app_id_str);

    std::string dll_path = std::string(dll_dir) + "\\steam_api64.dll";
    SetDllDirectoryA(dll_dir);
    HMODULE hSteam = LoadLibraryA(dll_path.c_str());
    if (!hSteam) {
        DeleteFileA(appid_file.c_str());
        return 1;
    }

    auto pfnInitFlat     = reinterpret_cast<PFN_SteamAPI_InitFlat>(
                               GetProcAddress(hSteam, "SteamAPI_InitFlat"));
    auto pfnShutdown     = reinterpret_cast<PFN_SteamAPI_Shutdown>(
                               GetProcAddress(hSteam, "SteamAPI_Shutdown"));
    auto pfnRunCallbacks = reinterpret_cast<PFN_SteamAPI_RunCallbacks>(
                               GetProcAddress(hSteam, "SteamAPI_RunCallbacks"));

    if (!pfnInitFlat || !pfnShutdown || !pfnRunCallbacks) {
        FreeLibrary(hSteam);
        DeleteFileA(appid_file.c_str());
        return 1;
    }

    char err_msg[1024]{};
    if (pfnInitFlat(err_msg) != 0) {
        FreeLibrary(hSteam);
        DeleteFileA(appid_file.c_str());
        return 1;
    }

    int check_tick = 0;
    while (true) {
        pfnRunCallbacks();
        Sleep(1000);

        if (++check_tick >= 60) {
            check_tick = 0;
            if (!is_process_running("WizardGraphicalClient.exe"))
                break;
        }
    }

    pfnShutdown();
    FreeLibrary(hSteam);
    DeleteFileA(appid_file.c_str());

    return 0;
}
