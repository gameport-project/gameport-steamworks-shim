#pragma once
// GamePort: the shim's debug lines go to logcat. A few of them repeat on every frame of a game (the callbacks the game pumps from its update loop, some forty
// to ninety times a second): they filled the log with hundreds of lines a second and cost the game thread a write each, so they are not written.
#ifdef GP_LOGCAT
#include <android/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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

static inline void gp_debug(const char *format, ...) __attribute__((format(printf, 1, 2)));
static inline void gp_debug(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (gp_repeats_every_frame(line)) return;
    __android_log_write(ANDROID_LOG_INFO, "GPSteam", line);
}
#endif
