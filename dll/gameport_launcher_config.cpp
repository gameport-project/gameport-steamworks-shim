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
// Protocol, in the game's gameport folder: the shim writes steam_ticket.request with an id; the hook
// writes steam_ticket.bin and then steam_ticket.ready carrying the same id.
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

bool gameport_read_ticket(std::vector<uint8_t> &out)
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
