// setups.cpp - see setups.h.
//
//   A command goes in three steps, a little apart, the way a hand would type it: the game's console
//   key pressed (its key down, its character, its key up), the command's characters, then Enter. Each
//   is a window message posted to the game's window, marked (lParam bit 25, which Windows leaves
//   unused) so the menu's window procedure passes them on while it keeps the real keyboard's presses
//   to itself. Then the game's own log (lastlog.txt in its personal folder) is read as it grows, for
//   its answer ("Setup updated", "Setup written", or what went wrong), for a few seconds.
#include "setups.h"

#include <windows.h>
#ifdef _WIN32
#include <share.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "log.h"
#include "paths.h"

namespace mxcam {
namespace setups {
namespace {

constexpr long long kMarker = 1ll << 25;  // (a WM_KEY*/WM_CHAR lParam bit Windows leaves unused)

std::vector<Setup> g_list;
double g_listedAt = -10.0;
std::string g_current;
char g_status[160] = "";

enum class Step { Idle, Console, Type, Enter, Wait };
Step        g_step = Step::Idle;
double      g_stepAt = 0;
long long   g_frame = 0, g_stepFrame = 0;  // Update's calls (frames), and the one the last step was taken in
std::string g_command, g_name;
bool        g_writing = false;
long long   g_logFrom = -1;  // lastlog.txt's size when the command was sent
std::string g_answer;

std::wstring Folder() { return std::wstring(paths::PersonalFolder()) + L"setups"; }

std::string Upper(std::string text)
{
    for (char& c : text)
        c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return text;
}

long long LogSize()
{
    WIN32_FILE_ATTRIBUTE_DATA info;
    std::wstring path = std::wstring(paths::PersonalFolder()) + L"lastlog.txt";
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
        return -1;
    return static_cast<long long>(info.nFileSizeHigh) << 32 | info.nFileSizeLow;
}

// What the game wrote to its log since the command (the lines with "setup" in them, or any error).
std::string LogSince(long long from)
{
    std::wstring path = std::wstring(paths::PersonalFolder()) + L"lastlog.txt";
    FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!f)
        return "";
    std::string text;
    if (from > 0)
        fseek(f, static_cast<long>(from), SEEK_SET);
    char buffer[4096];
    size_t got;
    while ((got = fread(buffer, 1, sizeof buffer, f)) > 0 && text.size() < 65536)
        text.append(buffer, got);
    fclose(f);
    std::string answer;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (!line.empty() && line[0] == '[') {  // "[1:21:18.13] Setup updated": its time left off
            const size_t close = line.find("] ");
            if (close != std::string::npos)
                line.erase(0, close + 2);
        }
        const std::string upper = Upper(line);
        if (upper.find("SETUP") != std::string::npos || upper.find("ERROR") != std::string::npos ||
            upper.find("UNKNOWN") != std::string::npos || upper.find("CAN'T") != std::string::npos ||
            upper.find("COULD") != std::string::npos)
            answer = line;
    }
    return answer;
}

#ifdef _WIN32
// A key pressed and let go: its key down and up only. The game's own message loop translates the key
// down into its character (TranslateMessage), as for a key on the keyboard - a character posted along
// with it arrived twice. (The first try typed "/writesetup test_1/writesetup test-1": every character
// posted, then each again as translated - with Shift up there, '_' as '-' - and the game wrote a file
// "writesetup test-1" in a new folder "test_1".) Its scan code and extended bit as the keyboard sends
// them (a right Ctrl, the keypad's Enter, the arrows).
void Press(HWND window, UINT vk, UINT scan = 0, bool extended = false)
{
    if (!scan)
        scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    const LPARAM ext = extended ? (1ll << 24) : 0;
    PostMessageW(window, WM_KEYDOWN, vk, static_cast<LPARAM>(1 | scan << 16 | kMarker) | ext);
    PostMessageW(window, WM_KEYUP, vk, static_cast<LPARAM>(1 | scan << 16 | kMarker | 0xC0000000ll) | ext);
}
#endif
}  // namespace

// The game's keys by the names its settings give them (its own table of key names): the key's virtual
// key, its scan code where two keys share one (Shift, Ctrl), and whether it's an extended key.
bool ReadGameKey(const char* setting, GameKey& out)
{
    out = GameKey{};
    const std::wstring path = std::wstring(paths::PersonalFolder()) + L"config";
    FILE* f = nullptr;
#ifdef _WIN32
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0)
        f = nullptr;
#else
    f = fopen(std::string(path.begin(), path.end()).c_str(), "rb");
#endif
    if (!f)
        return false;
    const std::string prefix = std::string(setting) + "=";
    char line[256];
    bool found = false;
    while (!found && fgets(line, sizeof line, f)) {
        if (strncmp(line, prefix.c_str(), prefix.size()) != 0)
            continue;
        found = true;
        std::string value = line + prefix.size();
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
            value.pop_back();
        out.name = value;
    }
    fclose(f);
    if (!found)
        return false;
    // "(k)<key>": a key or a button; else an axis ("(0.06,0.94)<device> axis 5").
    if (out.name.rfind("(k)", 0) != 0)
        return false;
    out.name.erase(0, 3);
#ifdef _WIN32
    if (out.name.size() == 1) {
        const SHORT scan = VkKeyScanA(out.name[0]);
        if (scan == -1)
            return false;
        out.vk = static_cast<unsigned>(scan & 0xFF);
        return true;
    }
    struct Named { const char* name; unsigned vk; unsigned scan; bool extended; };
    static const Named kNames[] = {
        { "Esc", VK_ESCAPE, 0, false },        { "Backspace", VK_BACK, 0, false },      { "Tab", VK_TAB, 0, false },
        { "Enter", VK_RETURN, 0, false },      { "Left Ctrl", VK_CONTROL, 0x1D, false }, { "Right Ctrl", VK_CONTROL, 0x1D, true },
        { "Left Shift", VK_SHIFT, 0x2A, false }, { "Right Shift", VK_SHIFT, 0x36, false }, { "Space", VK_SPACE, 0, false },
        { "Caps Lock", VK_CAPITAL, 0, false }, { "Insert", VK_INSERT, 0, true },        { "Delete", VK_DELETE, 0, true },
        { "Home", VK_HOME, 0, true },          { "End", VK_END, 0, true },              { "Page Up", VK_PRIOR, 0, true },
        { "Page Down", VK_NEXT, 0, true },     { "Up", VK_UP, 0, true },                { "Down", VK_DOWN, 0, true },
        { "Left", VK_LEFT, 0, true },          { "Right", VK_RIGHT, 0, true },          { "Pause", VK_PAUSE, 0, false },
        { "Num Lock", VK_NUMLOCK, 0, true },   { "Keypad /", VK_DIVIDE, 0, true },      { "Keypad *", VK_MULTIPLY, 0, false },
        { "Keypad -", VK_SUBTRACT, 0, false }, { "Keypad +", VK_ADD, 0, false },        { "Keypad Enter", VK_RETURN, 0, true },
        { "Keypad .", VK_DECIMAL, 0, false },
    };
    for (const Named& n : kNames)
        if (out.name == n.name) {
            out.vk = n.vk;
            out.scan = n.scan;
            out.extended = n.extended;
            return true;
        }
    if (out.name.size() >= 2 && out.name.size() <= 3 && out.name[0] == 'F' && isdigit(static_cast<unsigned char>(out.name[1]))) {
        const int n = atoi(out.name.c_str() + 1);
        if (n >= 1 && n <= 12) {
            out.vk = VK_F1 + (n - 1);
            return true;
        }
    }
    if (out.name.rfind("Keypad ", 0) == 0 && out.name.size() == 8 && isdigit(static_cast<unsigned char>(out.name[7]))) {
        out.vk = VK_NUMPAD0 + (out.name[7] - '0');
        return true;
    }
#endif
    return false;  // (a controller's button, a mouse button: not a key the mod can press)
}

namespace {

#ifdef _WIN32
// The game's console key (its settings: console_key=(k)<key>; the game writes them when it quits, so a
// key changed this session counts once the game has been restarted). T, the game's own, if it can't be
// told.
GameKey ConsoleKey()
{
    GameKey key;
    if (ReadGameKey("console_key", key))
        return key;
    static std::string told;
    if (key.name != told) {
        told = key.name;
        Log("setups: the console key \"%s\" isn't a keyboard key the mod can press: using T", key.name.c_str());
    }
    key = GameKey{};
    key.vk = 'T';
    key.name = "t";
    return key;
}

// Text: its characters alone (no key downs to be translated again), exactly as written - Shift and
// all, whatever the keyboard's state.
void TypeText(HWND window, const std::string& text)
{
    for (char c : text) {
        const SHORT scan = VkKeyScanA(c);
        const UINT vsc = scan != -1 ? MapVirtualKeyW(scan & 0xFF, MAPVK_VK_TO_VSC) : 0;
        PostMessageW(window, WM_CHAR, static_cast<WPARAM>(static_cast<unsigned char>(c)), static_cast<LPARAM>(1 | vsc << 16 | kMarker));
    }
}
#endif

bool Send(const std::string& name, bool writing, double now)
{
#ifdef _WIN32
    if (g_step != Step::Idle && g_step != Step::Wait)
        return false;
    if (writing) {  // the setups folder, the first time
        const std::wstring folder = Folder();
        if (GetFileAttributesW(folder.c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (CreateDirectoryW(folder.c_str(), nullptr))
                Log("setups: made the setups folder (%ls)", folder.c_str());
            else
                Log("setups: couldn't make the setups folder %ls (error %lu)", folder.c_str(), GetLastError());
        }
    }
    g_name = name;
    g_writing = writing;
    g_command = (writing ? "/writesetup " : "/readsetup ") + name;
    g_step = Step::Console;
    g_stepAt = now;
    g_stepFrame = g_frame;
    g_logFrom = LogSize();
    g_answer.clear();
    snprintf(g_status, sizeof g_status, "%s %s...", writing ? "SAVING" : "LOADING", Upper(name).c_str());
    Log("setups: typing \"%s\" into the game's console", g_command.c_str());
    return true;
#else
    (void)name;
    (void)writing;
    (void)now;
    snprintf(g_status, sizeof g_status, "SETUPS FROM THE MENU AREN'T AVAILABLE ON LINUX YET");
    return false;
#endif
}

// The setups folder is read on a thread of its own, never the game's: with many setups, or the folder
// somewhere slow to open files in (synced to the cloud, scanned by an antivirus), reading them all on
// the game's thread every second made the game stutter while the menu was open. A file's bike= line
// is read again only when the file changed (kept by its write time; only the scan's thread uses it).
struct KnownFile {
    long long   stamp = 0;
    std::string bike;
};

std::mutex         g_scanLock;
std::vector<Setup> g_scanned;          // (the scan's result, under g_scanLock)
bool               g_scanDone = false;  // (under g_scanLock)
std::atomic<bool>  g_scanning{ false };

void ScanSetups(std::wstring folder)
{
    static std::unordered_map<std::wstring, KnownFile> known;  // (one scan at a time: g_scanning)
    std::vector<Setup> list;
    try {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(folder), error)) {
            if (!entry.is_regular_file(error))
                continue;
            Setup s;
            s.name = entry.path().filename().u8string();  // (as UTF-8: never throws on a name the code page lacks)
            std::error_code timeError;
            const auto written = entry.last_write_time(timeError);
            const long long stamp = timeError ? 0 : static_cast<long long>(written.time_since_epoch().count());
            const std::wstring path = entry.path().wstring();
            const auto was = known.find(path);
            if (stamp != 0 && was != known.end() && was->second.stamp == stamp) {
                s.bike = was->second.bike;
            } else {
                if (FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO)) {
                    char line[256];
                    while (fgets(line, sizeof line, f)) {
                        if (strncmp(line, "bike=", 5) == 0) {
                            s.bike = line + 5;
                            while (!s.bike.empty() && (s.bike.back() == '\n' || s.bike.back() == '\r'))
                                s.bike.pop_back();
                            break;
                        }
                    }
                    fclose(f);
                }
                known[path] = KnownFile{ stamp, s.bike };
            }
            list.push_back(std::move(s));
        }
        std::sort(list.begin(), list.end(), [](const Setup& a, const Setup& b) { return Upper(a.name) < Upper(b.name); });
    } catch (...) {
        // (A name or a folder the file system calls couldn't take: what was listed so far stands.)
    }
    {
        std::lock_guard<std::mutex> hold(g_scanLock);
        g_scanned = std::move(list);
        g_scanDone = true;
    }
    g_scanning = false;
}

}  // namespace

const std::vector<Setup>& List(double now)
{
    {
        std::lock_guard<std::mutex> hold(g_scanLock);
        if (g_scanDone) {
            g_list.swap(g_scanned);
            g_scanDone = false;
        }
    }
    if (now - g_listedAt >= 1.0 && !g_scanning.load() && paths::PersonalFolder()[0]) {
        g_listedAt = now;
        g_scanning = true;
        try {
            std::thread(ScanSetups, Folder()).detach();
        } catch (...) {
            g_scanning = false;
        }
    }
    return g_list;
}

bool ValidName(const std::string& name)
{
    if (name.empty() || name.size() > 40)
        return false;
    for (char c : name)
        if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.')
            return false;
    return name != "." && name != "..";
}

bool Load(const std::string& name, double now) { return Send(name, false, now); }
bool Save(const std::string& name, double now) { return Send(name, true, now); }

void Update(void* windowHandle, double now)
{
#ifdef _WIN32
    HWND window = static_cast<HWND>(windowHandle);
    if (g_step == Step::Idle || !window)
        return;
    const double since = now - g_stepAt;
    ++g_frame;
    // Each step a frame after the last (the fastest it goes: the game takes in the window's messages
    // between its frames, so the console is open, its key's own character gone by, before the text is
    // typed): the console is up for only three or four frames - all but unseen.
    const bool nextFrame = g_frame > g_stepFrame;
    const auto Took = [&]() { g_stepAt = now; g_stepFrame = g_frame; };
    switch (g_step) {
    case Step::Console: {  // the console opened
        const GameKey key = ConsoleKey();
        static std::string logged;
        if (key.name != logged) {
            logged = key.name;
            Log("setups: the game's console key: \"%s\"", key.name.c_str());
        }
        Press(window, key.vk, key.scan, key.extended);
        g_step = Step::Type;
        Took();
        break;
    }
    case Step::Type:  // the command typed into it (a moment later: it's open)
        if (!nextFrame)
            break;
        TypeText(window, g_command);
        g_step = Step::Enter;
        Took();
        break;
    case Step::Enter:
        if (!nextFrame)
            break;
        Press(window, VK_RETURN);
        g_step = Step::Wait;
        Took();
        break;
    case Step::Wait: {  // the game's answer, in its log
        const std::string answer = LogSince(g_logFrom);
        if (!answer.empty() && answer != g_answer) {
            g_answer = answer;
            const std::string upper = Upper(answer);
            const bool done = upper.find(g_writing ? "WRITTEN" : "UPDATED") != std::string::npos;
            if (done) {
                g_current = g_name;
                g_listedAt = -10.0;  // (a new one in the list)
                snprintf(g_status, sizeof g_status, "%s %s", g_writing ? "SAVED YOUR SETUP AS" : "LOADED", Upper(g_name).c_str());
            } else {
                snprintf(g_status, sizeof g_status, "THE GAME SAYS: %s", upper.c_str());
            }
            Log("setups: the game answered \"%s\"", answer.c_str());
            // It answered: its console took the command and is still open - closed at once (Esc in it goes
            // back to the game; the user's test). Only after an answer: with none the console may never have
            // opened, and Esc out on the track opens the game's own menu.
            Press(window, VK_ESCAPE);
            g_step = Step::Idle;
        } else if (since > 4.0) {
            snprintf(g_status, sizeof g_status, "NO ANSWER FROM THE GAME - ITS CONSOLE MAY NOT BE OPEN HERE (TRY ON A TRACK)");
            Log("setups: no answer in the game's log for \"%s\"", g_command.c_str());
            g_step = Step::Idle;
        }
        break;
    }
    default:
        break;
    }
#else
    (void)windowHandle;
    (void)now;
#endif
}

const char* Status() { return g_status; }
bool Busy() { return g_step != Step::Idle; }
const std::string& Current() { return g_current; }
bool Injected(long long lParam) { return (lParam & kMarker) != 0; }

}  // namespace setups
}  // namespace mxcam
