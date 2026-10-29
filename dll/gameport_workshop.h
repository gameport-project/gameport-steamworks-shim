#pragma once
// GamePort: the Workshop items of a game, as the hook and GamePort download them (see gameport_launcher_config.cpp).
#include <stdint.h>
#include <string>
#include <vector>

struct GpWorkshopItem {
    uint64_t id = 0;
    std::string phase;      // QUEUED, DOWNLOADING, INSTALLED or FAILED
    float fraction = 0.f;   // 0..1 while downloading
    uint64_t sizeOnDisk = 0;
    std::string folder;
    std::string title;
    uint64_t creator = 0;
    uint32_t updated = 0;
    std::string error;
};

void gameport_trace(const char *format, ...) __attribute__((format(printf, 1, 2)));
std::vector<uint64_t> gameport_workshop_wanted();
void gameport_workshop_want(uint64_t id);
bool gameport_workshop_item(uint64_t id, GpWorkshopItem &out);

// Extra contents (DLC) the game asks to install: relayed the same way, in dlc.request and dlc.state.
void gameport_dlc_want(uint32_t appId);
bool gameport_dlc_state(uint32_t appId, std::string &phase, float &fraction);

// Items the game asked for during this run (as opposed to those kept from earlier runs): their outcome is told to the game.
std::vector<uint64_t> gameport_workshop_added_this_run();
// A web page the game opens in Steam's overlay: when it is the page of a Workshop item, the item is fetched, as if the game had subscribed to it.
void gameport_overlay_page(const char *url);

// Every item GamePort tells the game about in workshop.state (those the account is subscribed to, and those the game asked for): they count among the game's subscriptions.
std::vector<uint64_t> gameport_workshop_known();

// Where GamePort writes the lists of extra contents (DLC.txt, dlc_missing.txt): empty when it gave none. The game's own settings folder is in the app's library folder,
// which GamePort cannot write, so its lists were never read and every extra content was answered as owned and installed.
std::string gameport_dlc_settings_path();
// Whether an extra content is on the device: true for one in dlc_installed.txt, or that GamePort has just installed; true for all when GamePort gave no such list.
bool gameport_dlc_installed(uint32_t appId);

// Whether the account has an app (an extra content, in practice), from owned_apps.txt: [known] is false when GamePort gave no such list (then the older answer stands).
bool gameport_owns_app(uint32_t appId, bool &known);
