#pragma once
// GamePort: the shim's debug lines go to logcat. A few of them repeat on every frame of a game (the callbacks the game pumps from its update loop, some forty
// to ninety times a second): they filled the log with hundreds of lines a second and cost the game thread a write each, so they are not written.
#ifdef GP_LOGCAT
#include <android/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <map>
#include <mutex>
#include <string>

static inline bool gp_repeats_every_frame(const char *line)
{
    static const char *const prefixes[] = {
        "Steam_Client::RunCallbacks",
        "SteamAPI_ManualDispatch_Init",
        "SteamAPI_ManualDispatch_GetNextCallback",
        "SteamAPI_ManualDispatch_FreeLastCallback",
        "Steam_BGetCallback",
        "Steam_FreeLastCallback",
    };
    for (const char *prefix : prefixes) {
        if (strncmp(line, prefix, strlen(prefix)) == 0) return true;
    }
    return false;
}

// The Workshop and DLC calls of a game are polled many times a second: each distinct line is written once a second at most, so what the game asks for can be read.
static inline bool gp_throttled(const char *line)
{
    static std::mutex *lock = new std::mutex();
    static std::map<std::string, long long> *last = new std::map<std::string, long long>();
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    const long long now = (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
    std::lock_guard<std::mutex> guard(*lock);
    auto found = last->find(line);
    if (found != last->end() && now - found->second < 1000) return true;
    if (last->size() > 2000) last->clear();
    (*last)[line] = now;
    return false;
}

// By default only the lines that matter are written: what the game asks of the Workshop and of the extra contents, the page it opens in the overlay, and what GamePort provides. All the
// others (every call to Steam, the network of the emulator, the account's identity at the start) are written only when the file gameport/trace_all exists in the game's folder.
static inline bool gp_trace_all()
{
    static int answer = -1;
    if (answer < 0) {
        answer = 0;
        FILE *f = fopen("/proc/self/cmdline", "r");
        if (f) {
            char cmd[256] = {0};
            size_t n = fread(cmd, 1, sizeof(cmd) - 1, f);
            fclose(f);
            cmd[n] = 0;
            std::string path = std::string("/storage/emulated/0/Android/data/") + cmd + "/files/gameport/trace_all";
            answer = access(path.c_str(), F_OK) == 0 ? 1 : 0;
        }
    }
    return answer == 1;
}

static inline bool gp_important(const char *line)
{
    static const char *const prefixes[] = {
        "launcher config", "GamePort:", "Steam_UGC::", "workshop:", "ugc:", "dlc:", "overlay:", "Steam_Apps::InstallDLC", "Steam_Apps::GetDlcDownloadProgress",
        "Steam_Apps::GetDLCCount", "Locking all DLC", "Unlocking all DLC", "Adding DLC", "GetISteamUGC", "Steam_Friends::ActivateGameOverlay",
    };
    for (const char *prefix : prefixes) {
        if (strncmp(line, prefix, strlen(prefix)) == 0) return true;
    }
    return false;
}

static inline void gp_debug(const char *format, ...) __attribute__((format(printf, 1, 2)));
static inline void gp_debug(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (!gp_important(line) && !gp_trace_all()) return;
    if (gp_repeats_every_frame(line) || gp_throttled(line)) return;
    __android_log_write(ANDROID_LOG_INFO, "GPSteam", line);
}
#endif
