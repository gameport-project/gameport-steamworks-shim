#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <android/log.h>
#include <string>
#include <vector>

// GamePort provisions per-user data for the shim in one of two ways:
//  1. files under <game external files dir>/gameport/ (development and adb overrides), or
//  2. a small config baked into the patched APK as assets/gameport/steam.cfg (normal case), which
//     this file turns into the settings files Steamworks reads, once per launch.
// The baked config only holds public account data: app id, SteamID and display name.

static const char *kSavesDir = "Goldberg SteamEmu Saves";
static const char *kConfigEntry = "assets/gameport/steam.cfg";
// Optional, written at patch time: what Steam holds about the game's achievements (a JSON array in the format of
// steam_settings/achievements.json), and which of them the account had already unlocked (a JSON object, in the format
// of the save file of the unlocked achievements). A game patched before they existed simply has neither.
static const char *kAchievementsEntry = "assets/gameport/achievements.json";
static const char *kEarnedEntry = "assets/gameport/achievements_earned.json";

static bool read_first_line(const std::string &path, std::string &out)
{
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return false;
    char buf[64] = {0};
    bool ok = fgets(buf, sizeof(buf), f) != NULL;
    fclose(f);
    if (!ok) return false;
    out = buf;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return !out.empty();
}

static void make_dirs(const std::string &path)
{
    for (size_t i = 1; i < path.size(); i++) {
        if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0770);
    }
    mkdir(path.c_str(), 0770);
}

static void write_file(const std::string &path, const std::string &content)
{
    FILE *f = fopen(path.c_str(), "w");
    if (!f) return;
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
}

// Path of the APK this process runs from, taken from its memory mappings.
static bool find_apk_path(std::string &out, const std::string &package)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[1024];
    bool found = false;
    std::string fallback;
    while (!found && fgets(line, sizeof(line), f)) {
        char *slash = strchr(line, '/');
        if (!slash) continue;
        std::string path(slash);
        while (!path.empty() && (path.back() == '\n' || path.back() == ' ')) path.pop_back();
        size_t bang = path.find('!');
        if (bang != std::string::npos) path.resize(bang);
        if (path.size() > 9 && path.compare(path.size() - 9, 9, "/base.apk") == 0) {
            // Other base.apk files are mapped too (a system component's, for one): this process's own is the one under its package name.
            bool own = !package.empty() && path.find("/" + package + "-") != std::string::npos;
            if (own || fallback.empty()) fallback = path;
            if (own) found = true;
        }
    }
    fclose(f);
    if (!found && !fallback.empty()) found = true;
    if (found) out = fallback;
    return found;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

static bool read_at(FILE *f, uint64_t offset, void *buf, size_t len)
{
    return fseeko(f, (off_t)offset, SEEK_SET) == 0 && fread(buf, 1, len, f) == len;
}

// Reads one stored (uncompressed) entry out of a zip, without unpacking anything else.
static bool read_stored_entry(const std::string &zip_path, const char *entry, std::string &out)
{
    FILE *f = fopen(zip_path.c_str(), "rb");
    if (!f) return false;
    bool ok = false;

    fseeko(f, 0, SEEK_END);
    uint64_t size = (uint64_t)ftello(f);
    size_t tail_len = size < 70000 ? (size_t)size : 70000;
    std::vector<uint8_t> tail(tail_len);
    if (!read_at(f, size - tail_len, tail.data(), tail_len)) { fclose(f); return false; }

    // End of central directory record.
    long eocd = -1;
    for (long i = (long)tail_len - 22; i >= 0; i--) {
        if (rd32(&tail[i]) == 0x06054b50) { eocd = i; break; }
    }
    if (eocd < 0) { fclose(f); return false; }
    uint64_t cd_size = rd32(&tail[eocd + 12]);
    uint64_t cd_offset = rd32(&tail[eocd + 16]);
    if (cd_offset == 0xFFFFFFFFu || cd_size == 0xFFFFFFFFu) {
        // Zip64: the locator sits right before the end record and points at the zip64 end record.
        if (eocd < 20 || rd32(&tail[eocd - 20]) != 0x07064b50) { fclose(f); return false; }
        uint8_t z64[56];
        if (!read_at(f, rd64(&tail[eocd - 20 + 8]), z64, sizeof(z64)) || rd32(z64) != 0x06064b50) { fclose(f); return false; }
        cd_size = rd64(z64 + 40);
        cd_offset = rd64(z64 + 48);
    }

    std::vector<uint8_t> cd((size_t)cd_size);
    if (!read_at(f, cd_offset, cd.data(), cd.size())) { fclose(f); return false; }

    size_t want = strlen(entry);
    size_t pos = 0;
    while (pos + 46 <= cd.size() && rd32(&cd[pos]) == 0x02014b50) {
        uint16_t method = rd16(&cd[pos + 10]);
        uint64_t comp_size = rd32(&cd[pos + 20]);
        uint16_t name_len = rd16(&cd[pos + 28]);
        uint16_t extra_len = rd16(&cd[pos + 30]);
        uint16_t comment_len = rd16(&cd[pos + 32]);
        uint64_t local_offset = rd32(&cd[pos + 42]);
        if (pos + 46 + name_len + extra_len > cd.size()) break;

        if (name_len == want && memcmp(&cd[pos + 46], entry, want) == 0 && method == 0) {
            if (local_offset == 0xFFFFFFFFu) {
                // Offset lives in the zip64 extra field.
                const uint8_t *extra = &cd[pos + 46 + name_len];
                size_t e = 0;
                while (e + 4 <= extra_len) {
                    uint16_t id = rd16(extra + e), len = rd16(extra + e + 2);
                    if (id == 0x0001) {
                        size_t at = e + 4;
                        if (rd32(&cd[pos + 24]) == 0xFFFFFFFFu) at += 8;       // uncompressed size
                        if (rd32(&cd[pos + 20]) == 0xFFFFFFFFu) at += 8;       // compressed size
                        if (at + 8 <= e + 4 + len) local_offset = rd64(extra + at);
                        break;
                    }
                    e += 4 + len;
                }
            }
            uint8_t local[30];
            if (comp_size < (1u << 20) && read_at(f, local_offset, local, sizeof(local)) && rd32(local) == 0x04034b50) {
                uint64_t data_offset = local_offset + 30 + rd16(local + 26) + rd16(local + 28);
                out.resize((size_t)comp_size);
                ok = read_at(f, data_offset, &out[0], (size_t)comp_size);
            }
            break;
        }
        pos += 46 + name_len + extra_len + comment_len;
    }
    fclose(f);
    return ok;
}

static std::string config_value(const std::string &config, const char *key)
{
    std::string prefix = std::string(key) + "=";
    size_t start = 0;
    while (start < config.size()) {
        size_t end = config.find('\n', start);
        if (end == std::string::npos) end = config.size();
        if (config.compare(start, prefix.size(), prefix) == 0) {
            std::string value = config.substr(start + prefix.size(), end - start - prefix.size());
            while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
            return value;
        }
        start = end + 1;
    }
    return "";
}

static bool config_has_key(const std::string &config, const char *key)
{
    std::string prefix = std::string(key) + "=";
    return config.compare(0, prefix.size(), prefix) == 0 || config.find("\n" + prefix) != std::string::npos;
}

// "10,30,abc, 20" -> {"10","30","20"}: the numbers of a comma separated list, nothing else.
static std::vector<std::string> split_ids(const std::string &list)
{
    std::vector<std::string> ids;
    std::string current;
    for (size_t i = 0; i <= list.size(); i++) {
        char c = i < list.size() ? list[i] : ',';
        if (c >= '0' && c <= '9') current += c;
        else if (c == ',') {
            if (!current.empty()) ids.push_back(current);
            current.clear();
        } else current.clear();
    }
    return ids;
}

// The DLC the account has go to steam_settings/DLC.txt ("id=name"; its presence is also what stops the shim from saying yes to every DLC),
// the ones it is known not to have to steam_settings/dlc_missing.txt. A DLC on neither list keeps getting a yes. Without a "dlc" line
// (a game patched by an older GamePort) nothing is written and the shim says yes to every DLC, as it always did.
static std::string &gameport_base_dir() { static std::string *base = new std::string(); return *base; }

static void provision_dlc(const std::string &config, const std::string &base)
{
    std::string settings = base + "/steam_settings";
    if (!config_has_key(config, "dlc")) {
        unlink((settings + "/DLC.txt").c_str());
        unlink((settings + "/dlc_missing.txt").c_str());
        unsetenv("GP_DLC_UNLISTED_OWNED");
        return;
    }
    make_dirs(settings);
    std::string owned, missing;
    for (const std::string &id : split_ids(config_value(config, "dlc"))) owned += id + "=DLC " + id + "\n";
    for (const std::string &id : split_ids(config_value(config, "dlcmissing"))) missing += id + "\n";
    write_file(settings + "/DLC.txt", owned);
    write_file(settings + "/dlc_missing.txt", missing);
    setenv("GP_DLC_UNLISTED_OWNED", "1", 1);
}

// Writes the achievement definitions where the shim looks for them (see load_achievements_db), and seeds the file of the
// unlocked ones with what the account already has. Every step is optional and a failure leaves things as they were.
static void provision_achievements(const std::string &apk, const std::string &base, const std::string &appid)
{
    std::string definitions;
    if (!read_stored_entry(apk, kAchievementsEntry, definitions) || definitions.empty()) return;

    std::string settings = base + "/steam_settings";
    make_dirs(settings);
    write_file(settings + "/achievements.json", definitions);

    // The unlocked ones are only seeded once: after that the file belongs to the game's own unlocks, which are never overwritten.
    std::string earned;
    if (appid.empty() || !read_stored_entry(apk, kEarnedEntry, earned) || earned.empty()) return;
    std::string saves = base + "/" + kSavesDir + "/" + appid;
    make_dirs(saves);
    std::string target = saves + "/achievements.json";
    struct stat info;
    if (stat(target.c_str(), &info) != 0) write_file(target, earned);
}

// Turns the baked config into the files Steamworks reads.
static bool provision_from_apk(const std::string &base, const std::string &package)
{
    std::string apk;
    std::string config;
    // The hook tells where the APK is (it asks the system), which is surer than looking for it among the memory mappings.
    const char *known = getenv("GAMEPORT_APK");
    if (known && *known) apk = known;
    else if (!find_apk_path(apk, package)) {
        __android_log_print(ANDROID_LOG_WARN, "GPSteam", "launcher config: no APK found for %s", package.c_str());
        return false;
    }
    if (!read_stored_entry(apk, kConfigEntry, config)) {
        __android_log_print(ANDROID_LOG_WARN, "GPSteam", "launcher config: no %s in %s", kConfigEntry, apk.c_str());
        return false;
    }

    std::string appid = config_value(config, "appid");
    std::string steamid = config_value(config, "steamid");
    std::string name = config_value(config, "name");

    std::string settings = base + "/" + kSavesDir + "/settings";
    make_dirs(settings);
    if (!steamid.empty()) write_file(settings + "/user_steam_id.txt", steamid);
    if (!name.empty()) write_file(settings + "/account_name.txt", name);
    if (!appid.empty()) {
        write_file(base + "/appid.txt", appid);
        setenv("SteamAppId", appid.c_str(), 0);
        setenv("SteamGameId", appid.c_str(), 0);
    }
    provision_achievements(apk, base, appid);
    provision_dlc(config, base);
    __android_log_print(ANDROID_LOG_INFO, "GPSteam", "launcher config read: appid=%s, %s, family sharing %s", appid.c_str(),
        config_has_key(config, "dlc") ? "DLC lists given" : "no DLC lists", config_value(config, "familysharing") == "1" ? "yes" : "no");
    // Family Sharing, as GamePort knows it: asked by BIsSubscribedFromFamilySharing.
    if (config_value(config, "familysharing") == "1") setenv("GP_FAMILY_SHARED", "1", 1);
    else unsetenv("GP_FAMILY_SHARED");
    return true;
}

__attribute__((constructor)) static void gameport_load_launcher_config()
{
    char cmd[256] = {0};
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (!f) return;
    size_t n = fread(cmd, 1, sizeof(cmd) - 1, f);
    fclose(f);
    cmd[n] = 0;

    std::string base = std::string("/storage/emulated/0/Android/data/") + cmd + "/files/gameport";
    gameport_base_dir() = base;
    make_dirs(base);
    setenv("XDG_DATA_HOME", base.c_str(), 0);

    // The baked config is rewritten on every launch so it always matches the patched APK; without
    // one (an unpatched or hand-provisioned game), fall back to files placed under gameport/.
    if (provision_from_apk(base, cmd)) return;
    std::string appid;
    if (read_first_line(base + "/appid.txt", appid)) {
        setenv("SteamAppId", appid.c_str(), 0);
        setenv("SteamGameId", appid.c_str(), 0);
    }
}

// Asks the hook for a fresh Steam session ticket (a ticket is good for one use and goes stale), waits
// briefly for the answer, and returns it. Falls back to a ticket left at launch, then to false.
// Protocol, in the game's gameport folder: the shim writes steam_ticket.request with an id (and, on a
// second line, the service a web ticket is for); the hook writes steam_ticket.bin and then
// steam_ticket.ready carrying the same id.
#include <time.h>
#include <unistd.h>

static bool read_file_bytes(const std::string &path, std::vector<uint8_t> &out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t chunk[512];
    out.clear();
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) out.insert(out.end(), chunk, chunk + got);
    fclose(f);
    return !out.empty();
}

static bool gameport_read_ticket_blocking(std::vector<uint8_t> &out, const char *identity)
{
    char cmd[256] = {0};
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (!f) return false;
    size_t n = fread(cmd, 1, sizeof(cmd) - 1, f);
    fclose(f);
    cmd[n] = 0;

    const std::string dir = std::string("/storage/emulated/0/Android/data/") + cmd + "/files/gameport";
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    const std::string id = std::to_string((long long)now.tv_sec) + "-" + std::to_string((long long)now.tv_nsec);

    // Only ask when the hook is listening (it removes the request as soon as it sees it).
    FILE *request = fopen((dir + "/steam_ticket.request").c_str(), "w");
    if (request) {
        fputs(id.c_str(), request);
        if (identity && *identity) fprintf(request, "\n%s", identity);
        fclose(request);
        for (int waited = 0; waited < 10000; waited += 50) {
            std::vector<uint8_t> answer;
            if (read_file_bytes(dir + "/steam_ticket.ready", answer) && std::string(answer.begin(), answer.end()) == id) {
                if (read_file_bytes(dir + "/steam_ticket.bin", out)) return true;
                break;
            }
            usleep(50 * 1000);
        }
    }
    // No fresh ticket: whatever the hook left at launch, better than a placeholder.
    return read_file_bytes(dir + "/steam_ticket.bin", out);
}


// ---- Workshop items ------------------------------------------------------------------------------------------------------------------------------------------------
// The game cannot reach Steam's Workshop here. What it asks for is written to workshop.request (the ids, one per line, kept between runs); the hook relays it to GamePort,
// which downloads the items and leaves how far each one is in workshop.state (one line per item, tab separated: id, phase, fraction, size on disk, folder, title, maker,
// last update, error). Both files are in the game's gameport folder.
#include <android/log.h>
#include <stdarg.h>
#include <set>
#include <mutex>
#include <map>
#include "gameport_workshop.h"

static std::string workshop_dir()
{
    char cmd[256] = {0};
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (!f) return std::string();
    size_t n = fread(cmd, 1, sizeof(cmd) - 1, f);
    fclose(f);
    cmd[n] = 0;
    return std::string("/storage/emulated/0/Android/data/") + cmd + "/files/gameport";
}

void gameport_trace(const char *format, ...)
{
    char line[400];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    __android_log_write(ANDROID_LOG_INFO, "GPSteam", line);
}

static std::mutex workshop_lock;
static std::set<uint64_t> workshop_wanted_ids;
static bool workshop_loaded = false;
static std::set<uint64_t> workshop_added_run;

static void workshop_load_locked()
{
    if (workshop_loaded) return;
    workshop_loaded = true;
    std::vector<uint8_t> bytes;
    if (!read_file_bytes(workshop_dir() + "/workshop.request", bytes)) return;
    std::string text(bytes.begin(), bytes.end());
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        uint64_t id = strtoull(text.substr(start, end - start).c_str(), nullptr, 10);
        if (id) workshop_wanted_ids.insert(id);
        start = end + 1;
    }
}

std::vector<uint64_t> gameport_workshop_wanted()
{
    std::lock_guard<std::mutex> lock(workshop_lock);
    workshop_load_locked();
    return std::vector<uint64_t>(workshop_wanted_ids.begin(), workshop_wanted_ids.end());
}

void gameport_workshop_want(uint64_t id)
{
    if (!id) return;
    std::lock_guard<std::mutex> lock(workshop_lock);
    workshop_load_locked();
    if (!workshop_wanted_ids.insert(id).second) {
        // Asked for again in this run (the file already holds it): the game is told when it is there.
        workshop_added_run.insert(id);
        return;
    }
    workshop_added_run.insert(id);
    std::string text;
    for (uint64_t known : workshop_wanted_ids) text += std::to_string((unsigned long long)known) + "\n";
    const std::string dir = workshop_dir();
    const std::string tmp = dir + "/workshop.request.tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f) {
        gameport_trace("workshop: cannot write the request file (%s)", strerror(errno));
        return;
    }
    fputs(text.c_str(), f);
    fclose(f);
    rename(tmp.c_str(), (dir + "/workshop.request").c_str());
    gameport_trace("workshop: item %llu asked of GamePort (%zu wanted)", (unsigned long long)id, workshop_wanted_ids.size());
}

static std::map<uint64_t, GpWorkshopItem> workshop_items;

static long long now_ms()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static long long workshop_last_read = -100000;

static void workshop_refresh_locked()
{
    // Read at most five times a second: a game asks for the state of an item many times a frame.
    long long now = now_ms();
    if (now - workshop_last_read < 200) return;
    workshop_last_read = now;
    std::vector<uint8_t> bytes;
    if (!read_file_bytes(workshop_dir() + "/workshop.state", bytes)) return;
    std::string text(bytes.begin(), bytes.end());
    std::map<uint64_t, GpWorkshopItem> items;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        std::vector<std::string> f;
        size_t a = 0;
        for (;;) {
            size_t b = line.find('\t', a);
            if (b == std::string::npos) { f.push_back(line.substr(a)); break; }
            f.push_back(line.substr(a, b - a));
            a = b + 1;
        }
        if (f.size() < 8) continue;
        GpWorkshopItem item;
        item.id = strtoull(f[0].c_str(), nullptr, 10);
        item.phase = f[1];
        item.fraction = (float)atof(f[2].c_str());
        item.sizeOnDisk = strtoull(f[3].c_str(), nullptr, 10);
        item.folder = f[4];
        item.title = f[5];
        item.creator = strtoull(f[6].c_str(), nullptr, 10);
        item.updated = (uint32_t)strtoul(f[7].c_str(), nullptr, 10);
        item.error = f.size() > 8 ? f[8] : std::string();
        if (item.id) items[item.id] = item;
    }
    workshop_items.swap(items);
}

bool gameport_workshop_item(uint64_t id, GpWorkshopItem &out)
{
    std::lock_guard<std::mutex> lock(workshop_lock);
    workshop_refresh_locked();
    auto found = workshop_items.find(id);
    if (found == workshop_items.end()) return false;
    out = found->second;
    return true;
}


// ---- Tickets without stalling the game -----------------------------------------------------------------------------------------------------------------------------
// A game asks for a ticket from its own threads, some every few seconds, and the answer took about a second (the hook asks GamePort, which asks Steam): the game stopped for that
// second each time. A ticket is now made ahead, right after one is given out, and given at once when asked for; one older than two minutes is not trusted and is made anew.
#include <thread>
#include <chrono>

namespace {
struct PreparedTicket {
    std::vector<uint8_t> bytes;
    std::chrono::steady_clock::time_point madeAt;
    bool refreshing = false;
};
std::mutex &ticket_cache_lock() { static std::mutex *m = new std::mutex(); return *m; }
std::mutex &ticket_protocol_lock() { static std::mutex *m = new std::mutex(); return *m; }
std::map<std::string, PreparedTicket> &ticket_cache() { static auto *c = new std::map<std::string, PreparedTicket>(); return *c; }
const int kTicketMaxAgeSeconds = 120;

void refresh_ticket_later(const std::string key, const std::string identity)
{
    std::thread([key, identity]() {
        std::vector<uint8_t> fresh;
        bool ok;
        {
            std::lock_guard<std::mutex> protocol(ticket_protocol_lock());
            ok = gameport_read_ticket_blocking(fresh, identity.empty() ? nullptr : identity.c_str());
        }
        std::lock_guard<std::mutex> lock(ticket_cache_lock());
        PreparedTicket &entry = ticket_cache()[key];
        entry.refreshing = false;
        if (ok) {
            entry.bytes = std::move(fresh);
            entry.madeAt = std::chrono::steady_clock::now();
        }
    }).detach();
}
}

bool gameport_read_ticket(std::vector<uint8_t> &out, const char *identity)
{
    const std::string name = identity ? identity : "";
    {
        std::lock_guard<std::mutex> lock(ticket_cache_lock());
        PreparedTicket &entry = ticket_cache()[name];
        const bool fresh = !entry.bytes.empty() &&
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - entry.madeAt).count() < kTicketMaxAgeSeconds;
        if (fresh) {
            out = std::move(entry.bytes);
            entry.bytes.clear();
            if (!entry.refreshing) {
                entry.refreshing = true;
                refresh_ticket_later(name, name);
            }
            return true;
        }
        entry.bytes.clear();
    }
    bool ok;
    {
        std::lock_guard<std::mutex> protocol(ticket_protocol_lock());
        ok = gameport_read_ticket_blocking(out, identity);
    }
    if (ok) {
        std::lock_guard<std::mutex> lock(ticket_cache_lock());
        PreparedTicket &entry = ticket_cache()[name];
        if (!entry.refreshing) {
            entry.refreshing = true;
            refresh_ticket_later(name, name);
        }
    }
    return ok;
}

// ---- Extra contents the game asks to install ---------------------------------------------------------------------------------------------------------------------
static std::set<uint32_t> dlc_wanted_ids;
static std::map<uint32_t, std::pair<std::string, float>> dlc_states;
static long long dlc_last_read = -100000;

void gameport_dlc_want(uint32_t appId)
{
    if (!appId) return;
    std::lock_guard<std::mutex> lock(workshop_lock);
    if (!dlc_wanted_ids.insert(appId).second) return;
    std::string text;
    for (uint32_t known : dlc_wanted_ids) text += std::to_string((unsigned long long)known) + "\n";
    const std::string dir = workshop_dir();
    const std::string tmp = dir + "/dlc.request.tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f) {
        gameport_trace("dlc: cannot write the request file (%s)", strerror(errno));
        return;
    }
    fputs(text.c_str(), f);
    fclose(f);
    rename(tmp.c_str(), (dir + "/dlc.request").c_str());
    gameport_trace("dlc: extra content %u asked of GamePort", (unsigned)appId);
}

bool gameport_dlc_state(uint32_t appId, std::string &phase, float &fraction)
{
    std::lock_guard<std::mutex> lock(workshop_lock);
    long long now = now_ms();
    if (now - dlc_last_read >= 200) {
        dlc_last_read = now;
        std::vector<uint8_t> bytes;
        if (read_file_bytes(workshop_dir() + "/dlc.state", bytes)) {
            std::string text(bytes.begin(), bytes.end());
            std::map<uint32_t, std::pair<std::string, float>> states;
            size_t start = 0;
            while (start < text.size()) {
                size_t end = text.find('\n', start);
                if (end == std::string::npos) end = text.size();
                std::string line = text.substr(start, end - start);
                start = end + 1;
                size_t a = line.find('\t');
                if (a == std::string::npos) continue;
                size_t b = line.find('\t', a + 1);
                uint32_t id = (uint32_t)strtoul(line.substr(0, a).c_str(), nullptr, 10);
                std::string ph = line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
                float fr = b == std::string::npos ? 0.f : (float)atof(line.substr(b + 1).c_str());
                if (id) states[id] = std::make_pair(ph, fr);
            }
            dlc_states.swap(states);
        }
    }
    auto found = dlc_states.find(appId);
    if (found == dlc_states.end()) return false;
    phase = found->second.first;
    fraction = found->second.second;
    return true;
}


std::vector<uint64_t> gameport_workshop_added_this_run()
{
    std::lock_guard<std::mutex> lock(workshop_lock);
    return std::vector<uint64_t>(workshop_added_run.begin(), workshop_added_run.end());
}

// The page of a Workshop item opened in Steam's overlay is where a player subscribes to it: here that is a fetch.
static void gameport_open_in_browser(const std::string &url);

void gameport_overlay_page(const char *url)
{
    if (!url) return;
    gameport_trace("overlay: the game opens %s", url);
    const std::string text(url);
    gameport_open_in_browser(text);
    static const char *const markers[] = {"filedetails/?id=", "filedetails?id=", "CommunityFilePage/", "&id=", "?id="};
    for (const char *marker : markers) {
        size_t at = text.find(marker);
        if (at == std::string::npos) continue;
        at += strlen(marker);
        uint64_t id = strtoull(text.c_str() + at, nullptr, 10);
        if (id > 1000000) {
            gameport_workshop_want(id);
            return;
        }
    }
}


std::vector<uint64_t> gameport_workshop_known()
{
    std::lock_guard<std::mutex> lock(workshop_lock);
    workshop_refresh_locked();
    std::vector<uint64_t> ids;
    for (const auto &entry : workshop_items) ids.push_back(entry.first);
    return ids;
}

// A page of Steam the game opens in its overlay is opened in the headset's browser: the hook does it when it finds the address in browse.request.
static void gameport_open_in_browser(const std::string &url)
{
    const bool steam = url.rfind("https://steamcommunity.com/", 0) == 0 || url.rfind("https://store.steampowered.com/", 0) == 0;
    if (!steam) return;
    const std::string dir = workshop_dir();
    const std::string tmp = dir + "/browse.request.tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f) return;
    fputs(url.c_str(), f);
    fclose(f);
    rename(tmp.c_str(), (dir + "/browse.request").c_str());
    gameport_trace("overlay: the page is opened in the browser");
}


std::string gameport_dlc_settings_path()
{
    const std::string base = gameport_base_dir();
    if (base.empty()) return std::string();
    const std::string folder = base + "/steam_settings/";
    struct stat info;
    return stat((folder + "DLC.txt").c_str(), &info) == 0 ? folder : std::string();
}

static std::mutex dlc_installed_lock;
static long long dlc_installed_read = -100000;
static bool dlc_installed_given = false;
static std::set<uint32_t> dlc_installed_ids;
static std::set<uint32_t> dlc_content_ids;

bool gameport_dlc_installed(uint32_t appId)
{
    {
        std::lock_guard<std::mutex> lock(dlc_installed_lock);
        const long long now = now_ms();
        if (now - dlc_installed_read >= 2000) {
            dlc_installed_read = now;
            std::vector<uint8_t> bytes;
            dlc_installed_given = read_file_bytes(gameport_base_dir() + "/steam_settings/dlc_installed.txt", bytes);
            dlc_installed_ids.clear();
            dlc_content_ids.clear();
            if (dlc_installed_given) {
                std::string text(bytes.begin(), bytes.end());
                size_t start = 0;
                while (start < text.size()) {
                    size_t end = text.find('\n', start);
                    if (end == std::string::npos) end = text.size();
                    std::string line = text.substr(start, end - start);
                    start = end + 1;
                    if (line.empty() || line[0] == '#') continue;
                    const bool content = line.size() > 2 && line[0] == 'c' && line[1] == ':';
                    const bool onDevice = line.size() > 2 && line[0] == 'i' && line[1] == ':';
                    uint32_t id = (uint32_t)strtoul(line.c_str() + ((content || onDevice) ? 2 : 0), nullptr, 10);
                    if (!id) continue;
                    if (content) dlc_content_ids.insert(id); else dlc_installed_ids.insert(id);
                }
            }
        }
        if (!dlc_installed_given) return true;
        // Only an extra content with files of its own can be missing from the device: the others are unlocks, and are answered as they always were.
        if (!dlc_content_ids.count(appId)) return true;
        if (dlc_installed_ids.count(appId)) return true;
    }
    // One GamePort installed while the game runs (asked for by the game: see InstallDLC).
    std::string phase;
    float fraction = 0.f;
    return gameport_dlc_state(appId, phase, fraction) && phase == "INSTALLED";
}


static std::mutex owned_apps_lock;
static long long owned_apps_read = -100000;
static bool owned_apps_given = false;
static std::set<uint32_t> owned_apps_ids;

bool gameport_owns_app(uint32_t appId, bool &known)
{
    std::lock_guard<std::mutex> lock(owned_apps_lock);
    const long long now = now_ms();
    if (now - owned_apps_read >= 5000) {
        owned_apps_read = now;
        std::vector<uint8_t> bytes;
        owned_apps_given = read_file_bytes(gameport_base_dir() + "/steam_settings/owned_apps.txt", bytes);
        owned_apps_ids.clear();
        if (owned_apps_given) {
            std::string text(bytes.begin(), bytes.end());
            size_t start = 0;
            while (start < text.size()) {
                size_t end = text.find('\n', start);
                if (end == std::string::npos) end = text.size();
                uint32_t id = (uint32_t)strtoul(text.substr(start, end - start).c_str(), nullptr, 10);
                start = end + 1;
                if (id) owned_apps_ids.insert(id);
            }
        }
    }
    known = owned_apps_given;
    return owned_apps_given && owned_apps_ids.count(appId) > 0;
}
