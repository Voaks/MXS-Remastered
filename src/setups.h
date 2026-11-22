// setups.h
//   Bike setups from the mod menu (SETUPS tab): the game keeps them as text files in its personal folder
//   (%LOCALAPPDATA%\MX Simulator\setups\<name>) and loads or writes one with its console commands
//   /readsetup <name> and /writesetup <name>. Those are typed into the game's console for you, as a
//   keyboard would (its console key, the command, Enter) - the setup itself is the game's to read and
//   apply; nothing of the game's is changed or read but its files. The setups folder is made when the
//   first setup is written. The game's answer is read from its own log (lastlog.txt).
#pragma once

#include <string>
#include <vector>

namespace mxcam {
namespace setups {

struct Setup {
    std::string name;  // its file's name (what the commands take)
    std::string bike;  // its bike= line ("" if none)
};

// The setups in the game's setups folder, by name (read again at most once a second).
const std::vector<Setup>& List(double now);

// Whether name can be a new setup's: 1 to 40 letters, digits, '-', '_' or '.'.
bool ValidName(const std::string& name);

// Has the game load the setup, or write your bike's setup now under name (the setups folder made
// first). False: not now (a command still going, or no game window yet).
bool Load(const std::string& name, double now);
bool Save(const std::string& name, double now);

// Once a frame, with the game's window: types the command along, then watches the game's log for
// its answer.
void Update(void* window, double now);

// What became of the last command (upper case, for the menu; "" before the first), and whether one
// is still going.
const char* Status();
bool Busy();

// The setup last loaded or saved this session ("" none), to mark it in the list.
const std::string& Current();

// A setup renamed, or deleted: its file in your setups folder (no console needed; never another
// setup written over). Null if done, else why not (upper case, for the menu).
const char* Rename(const std::string& from, const std::string& to);
const char* Delete(const std::string& name);

// Setup codes, to share a setup as one line of text ("MXS-REMASTERED-SET-..."): its bike and its tuning
// (suspension, steering, gearing, brakes, the rider's weight...) - never the rider's own: their name,
// number, gear and graphics, nor their controls and aids (reversed steering, sticky dab, the assists).
// SetupCode: the code of one of your setups ("" if it can't be read).
std::string SetupCode(const std::string& name);

// The first setup code in text (a whole message pasted is fine), saved as a new setup in your setups
// folder: under wantedName if that's a valid name (else "shared"), a number added if it's taken (none of
// yours is ever written over). Your own name, number, gear, graphics, controls and the settings that are
// yours (kPersonal) go into it from your bike as it is now: the game writes your setup out first
// (/writesetup, on a track: Reading - the result in Status() once it's done). Load it as any other.
struct PastedSetup {
    enum Status { NotFound, Damaged, Newer, NoOwnSetup, Failed, Reading, Saved } status = NotFound;
    std::string bike;   // the bike it's for (its bike= line)
    std::string name;   // what it was saved as
};
PastedSetup PasteSetupCode(const char* text, const std::string& wantedName, double now);

// The same at once, your own lines from ownFile (a setup file of yours): for tests.
PastedSetup PasteSetupCodeFrom(const char* text, const std::string& wantedName, const std::wstring& ownFile);

// A key of the game's own, from its settings (its config file: "<setting>=(k)<key>"): a character ("t",
// ",") or one of the game's key names ("F2", "Enter", "Right Ctrl", "Keypad 5", ...). False: not set,
// or not a keyboard key (a controller's or the mouse's button); name: what the settings say either way.
// (The game writes its settings when it quits: a key changed this session is read once it's restarted.)
struct GameKey {
    unsigned    vk = 0;          // its virtual key (Windows)
    unsigned    scan = 0;        // its scan code, where two keys share the virtual key (Shift, Ctrl); 0 else
    bool        extended = false;
    std::string name;
};
bool ReadGameKey(const char* setting, GameKey& out);

// For the menu's window procedure: a key message the mod posted itself (let through to the game
// while the menu keeps the keyboard's to itself).
bool Injected(long long lParam);

}  // namespace setups
}  // namespace mxcam
