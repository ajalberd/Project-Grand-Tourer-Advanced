// gt6_menu.h - the in-game menu (F2): credit lock, career and licence unlock, PP lock, and the menu's own settings.
//
// Two halves:
//   gt6_menu_engine.cpp  the menu's engine on a thread of the game: a client of the game bridge (gt6_trainer.cpp) -
//                        its control pages and the heartbeat at P+0x44.
//   gt6_menu_ui.cpp      the F2 menu (Dear ImGui on the live renderer) that edits the settings below.
// The settings (one mutex) are the menu's; the engine copies them at the start of a tick and publishes a Status.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// [GT6 0087] The menu's language (2026-11-22: the port goes public - everything in English). Every Korean
// text of the menu stands in T_("..."): the Korean as written, or - while the language is English - its English out
// of gt6_menu_l10n.inc (made by tools\l10n; a text that is not in the table stays Korean). Settings key "MenuLang":
// 0 = by Windows' display language (Korean on a Korean Windows, English everywhere else), 1 = Korean, 2 = English.
// The language is fixed for a run of the game when the first text is asked for - tables of texts are made before the
// settings are loaded -, read from the settings file as it stands on the disk; a change holds from the next start.
const char* gt6_tr(const char* ko);
bool gt6_tr_english();
#define T_(s) gt6_tr(s)

namespace gt6menu {

struct PPProfile {
    int garageId = 0, carCode = 0;
    std::string label;
    float lockedPP = 0;
    bool enabled = true;
    float realPPWhenSaved = 0;
    std::string saved;
};


// The menu's settings, kept in GT6Menu.settings.json beside the game.
struct Settings {
    long long creditsLockValue = 48144100;
    bool creditsLock = false;
    bool unlockCareer = false;
    std::vector<PPProfile> ppProfiles;
    // the menu's own
    bool master = true;            // the built-in trainer as a whole (off = the game runs as without a trainer)
    bool padBlock = true;          // while the menu is open the game gets a neutral pad
    float uiScale = 1.0f;
    bool fullHd = false;           // the game's own 1080p mode from the next start on (main.cpp reads the file before the game boots)
    int realRes = 0;               // [GT6 0102] with fullHd: the game's 1440x1080 buffers drawn on the host at 0 = 1440x1080 (stretched,
                                   // as before), 1 = 1920x1080, 2 = 2560x1440, 3 = 3840x2160 - a real resolution (from the next start)
    bool camLoose = false;         // [GT6 0113] Looser Chase Camera (public issue #7; the RPCS3 patch by Edo for 1.22): the chase
    double camSpring = 90.0;       // camera's spring damper (the game's 300) and its largest angle (the game's 30) - gt6_trainer.cpp
    double camAngle = 35.0;        // chase_cam, live
    bool lanMode = false;          // [GT6 0114] LAN play without PSN (from the next start; main.cpp gt6_menu_setting_lan_mode)
    double screenBright = 1.0;     // Brightness (2026-11-24, a player's report: "image is very dim"): the picture goes to the window as
                                   // out = in^(1 / this) - 1 = as the game draws it, live [GT6 0101]
    int menuLang = 0;              // [GT6 0087] 0 = by Windows' display language, 1 = Korean, 2 = English (from the next start)
    int lastPage = 0;
};

unsigned toastSeq();               // counts the engine's / menu's messages (cheap: for the closed menu's notice)


struct RecentCar {
    int garageId = 0, carCode = 0;
    float pp = 0, ppTire = 0;
    std::string name;
    uint64_t seen = 0;             // GetTickCount64
};



struct Status {
    bool supported = false;        // the native bridge is ready for this game image
    bool external = false;         // the external trainer's heartbeat is in control: this engine stands back
    bool running = false;          // this engine holds the heartbeat
    int inRace = -1;               // -1 not known, 0 menus / garage, 1 a race is loaded
    bool cashFound = false;
    long long cash = 0;
    int rulesOverflow = 0, rulesCount = 0;
    std::vector<RecentCar> recent;
    // scans (diagnostic)
    double lastScanMs = 0;
    unsigned scans = 0;
    // messages of the engine for the status line (newest last)
    unsigned toastSeq = 0;
    std::string toast;
    bool toastWarn = false;
};


struct Command {
    enum Kind {
        SetCashOnce,
        FillNames,
    } kind;
    long long a = 0, b = 0;
    std::string s;
    std::vector<int> values;
};

// ---- shared state (gt6_menu_engine.cpp) ---------------------------------------------------------------------
std::mutex& mx();                  // guards settings() and edit(); never held by the engine across a tick
Settings& settings();              // the menu edits it under mx() and calls markDirty()
void markDirty();                  // the settings changed: the engine saves them
void rulesDirty();                 // ... and writes the SQL rules again
Status status();                   // a copy
void post(Command c);
std::vector<std::pair<int, std::string>> carNames();   // every car name the game has shown since boot
std::string carName(int code);     // "" = the game has not shown it yet
void toast(const std::string& text, bool warn = false);

void engineStart();                // gt6_menu_install
void padSample(unsigned port, unsigned buttons);   // cellPad hook: what the game would get
void menuCaptures(bool on);        // the menu takes the pad: the hot keys (R1+X, L1) see nothing

}  // namespace gt6menu

#ifdef __cplusplus
extern "C" {
#endif
void gt6_menu_install(void);       // main.cpp, after gt6_trainer_init. GT6_TRAINER_MENU=0 = not installed.
#ifdef __cplusplus
}
#endif
