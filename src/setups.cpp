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
#include <cmath>
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

// Your own setup as your bike has it, written by the game for a moment while a shared setup is pasted
// (its own lines taken from it), then deleted. Never shown in the list.
constexpr char    kOwnSetupName[] = "mxs_remastered_yours";
constexpr wchar_t kOwnSetup[]     = L"mxs_remastered_yours";

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
            if (s.name == kOwnSetupName)
                continue;  // (a paste's moment: your own setup, read and deleted)
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

// ---- Setup codes ---------------------------------------------------------------------------------
// "MXS-REMASTERED-SET-", then in base64url (as the preset codes): a version byte; the bike (its length,
// its name); each tuning line - a byte for which (its place in kTuning; 127: named in full, its length
// and name following; +128: its value is text, its length and text following, else a little-endian
// float); a checksum byte (a code cut short or mistyped isn't loaded).
namespace {

constexpr char    kSetupPrefix[] = "MXS-REMASTERED-SET-";
// (Version 2: the setup's own name after the bike - pasted under it. Version 1 codes, without it, still
// load: as "shared".)
constexpr uint8_t kSetupVersion  = 2;
constexpr char    kSetupBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
// The game's tuning lines, in the order its setup files have them. (Added to at the end only: codes
// already shared name lines by their place here.)
constexpr const char* kTuning[] = {
    "stability", "leveling", "roll_damping", "roll_limit_angle", "roll_limit_factor", "steering_damping",
    "steering_strength", "max_steering_force", "direct_steering", "slow_damping_speed", "slow_damping_factor",
    "land_damping_rampup", "land_damping_falloff", "land_damping_factor", "full_lock_speed", "restricted_lock_speed",
    "restricted_lock", "auto_clutch_rpms", "brake_torque", "rear_brake_torque", "max_dab_speed",
    "front_sprocket_adjustment", "rear_sprocket_adjustment", "fork_compression_adjustment",
    "fork_slow_compression_adjustment", "fork_rebound_adjustment", "fork_slow_rebound_adjustment",
    "fork_spring_rate_adjustment", "fork_preload_adjustment", "fork_oil_level_adjustment",
    "shock_compression_adjustment", "shock_slow_compression_adjustment", "shock_rebound_adjustment",
    "shock_slow_rebound_adjustment", "shock_spring_rate_adjustment", "shock_preload_adjustment", "rider_yspring",
    "rider_ydamping", "rider_zspring", "rider_zdamping", "rider_mass_distribution",
};
constexpr int kTuningCount = static_cast<int>(sizeof kTuning / sizeof kTuning[0]);
// The rider's own lines, never in a code: who they are and what they look like, and how they like to
// ride (controls, assists) - and, the user's choice, their stability, auto clutch RPMs, braking torques
// and max dab speed (still in kTuning: codes already made name them, and they're left out on paste).
constexpr const char* kPersonal[] = {
    "name", "number", "rider_texture", "head_texture", "bike_texture", "wheel_texture", "arcade", "front_brake",
    "rear_brake", "stop_wheelies", "auto_shift", "auto_dab", "auto_align", "sticky_dab", "reverse_steering",
    "lean_indicator", "stability", "auto_clutch_rpms", "brake_torque", "rear_brake_torque", "max_dab_speed",
};

uint8_t SetupChecksum(const std::vector<uint8_t>& bytes)
{
    uint8_t sum = 0x5A;
    for (uint8_t b : bytes)
        sum = static_cast<uint8_t>((sum << 1 | sum >> 7) ^ b);
    return sum;
}

bool IsPersonal(const std::string& key)
{
    for (const char* k : kPersonal)
        if (key == k)
            return true;
    return false;
}

// A setup file's lines, key and value, in order.
std::vector<std::pair<std::string, std::string>> ReadSetupFile(const std::wstring& path, bool& ok)
{
    std::vector<std::pair<std::string, std::string>> lines;
    ok = false;
    FILE* f = nullptr;
#ifdef _WIN32
    f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
#else
    f = fopen(std::string(path.begin(), path.end()).c_str(), "rb");
#endif
    if (!f)
        return lines;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n'))
            s.pop_back();
        const size_t eq = s.find('=');
        if (eq == std::string::npos || eq == 0)
            continue;
        lines.emplace_back(s.substr(0, eq), s.substr(eq + 1));
    }
    fclose(f);
    ok = true;
    return lines;
}

// A number as the game writes it in its setups ("%f": 15.000000), if the text is one through and through.
bool AsNumber(const std::string& text, float& out)
{
    if (text.empty())
        return false;
    char* end = nullptr;
    const double v = strtod(text.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(v))
        return false;
    out = static_cast<float>(v);
    return true;
}

void PutText(std::vector<uint8_t>& bytes, const std::string& text)
{
    const size_t n = std::min<size_t>(text.size(), 255);
    bytes.push_back(static_cast<uint8_t>(n));
    bytes.insert(bytes.end(), text.begin(), text.begin() + static_cast<std::ptrdiff_t>(n));
}

}  // namespace

std::string SetupCode(const std::string& name)
{
    if (name.empty() || name.find_first_of("/\\") != std::string::npos || name == "." || name == ".." || !paths::PersonalFolder()[0])
        return std::string();
    bool ok = false;
    const auto lines = ReadSetupFile((std::filesystem::path(Folder()) / std::filesystem::u8path(name)).wstring(), ok);
    if (!ok)
        return std::string();
    std::string bike;
    for (const auto& kv : lines)
        if (kv.first == "bike")
            bike = kv.second;
    std::vector<uint8_t> bytes{ kSetupVersion };
    PutText(bytes, bike);
    PutText(bytes, name);
    for (const auto& kv : lines) {
        if (kv.first == "bike" || IsPersonal(kv.first))
            continue;
        int index = 127;
        for (int i = 0; i < kTuningCount; ++i)
            if (kv.first == kTuning[i])
                index = i;
        float number = 0;
        const bool numeric = AsNumber(kv.second, number);
        bytes.push_back(static_cast<uint8_t>(index | (numeric ? 0 : 0x80)));
        if (index == 127)
            PutText(bytes, kv.first);
        if (numeric) {
            uint32_t bits;
            std::memcpy(&bits, &number, 4);
            for (int k = 0; k < 4; ++k)
                bytes.push_back(static_cast<uint8_t>(bits >> (8 * k)));
        } else {
            PutText(bytes, kv.second);
        }
    }
    bytes.push_back(SetupChecksum(bytes));
    std::string code = kSetupPrefix;
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint32_t n = bytes[i] << 16 | (i + 1 < bytes.size() ? bytes[i + 1] << 8 : 0) | (i + 2 < bytes.size() ? bytes[i + 2] : 0);
        const size_t chars = bytes.size() - i >= 3 ? 4 : bytes.size() - i + 1;
        for (size_t k = 0; k < chars; ++k)
            code += kSetupBase64[(n >> (18 - 6 * k)) & 63];
    }
    return code;
}

namespace {
// A setup code, read: its bike and its tuning lines (never a personal one, whatever it says).
struct DecodedSetup {
    std::string                                      bike;
    std::string                                      name;  // the setup's own name (version 2 on; "" before)
    std::vector<std::pair<std::string, std::string>> tuning;
};

PastedSetup::Status DecodeSetup(const char* text, DecodedSetup& out)
{
    DecodedSetup& result = out;
    result = DecodedSetup{};
    const char* start = text ? strstr(text, kSetupPrefix) : nullptr;
    if (!start)
        return PastedSetup::NotFound;
    start += sizeof kSetupPrefix - 1;
    std::vector<uint8_t> bytes;
    uint32_t n = 0;
    int bits = 0;
    for (const char* c = start; *c; ++c) {
        const char* at = strchr(kSetupBase64, *c);
        if (!at || !*c)
            break;
        n = n << 6 | static_cast<uint32_t>(at - kSetupBase64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<uint8_t>(n >> bits));
            n &= (1u << bits) - 1;
        }
    }
    if (bytes.size() < 3)
        return PastedSetup::Damaged;
    const uint8_t sum = bytes.back();
    bytes.pop_back();
    if (SetupChecksum(bytes) != sum)
        return PastedSetup::Damaged;
    if (bytes[0] < 1 || bytes[0] > kSetupVersion)
        return PastedSetup::Newer;
    const uint8_t version = bytes[0];
    // Its lines.
    size_t at = 1;
    const auto Text = [&](std::string& out) {
        if (at >= bytes.size() || at + 1 + bytes[at] > bytes.size())
            return false;
        out.assign(reinterpret_cast<const char*>(&bytes[at + 1]), bytes[at]);
        at += 1 + bytes[at];
        return true;
    };
    if (!Text(result.bike) || (version >= 2 && !Text(result.name)))
        return PastedSetup::Damaged;
    while (at < bytes.size()) {
        const uint8_t which = bytes[at++];
        const int index = which & 0x7F;
        std::string key, value;
        if (index == 127) {
            if (!Text(key))
                return PastedSetup::Damaged;
        } else if (index < kTuningCount) {
            key = kTuning[index];
        } else {
            return PastedSetup::Newer;  // (a line this version doesn't know the place of)
        }
        if (which & 0x80) {
            if (!Text(value))
                return PastedSetup::Damaged;
        } else {
            if (at + 4 > bytes.size())
                return PastedSetup::Damaged;
            uint32_t raw = 0;
            for (int k = 0; k < 4; ++k)
                raw |= static_cast<uint32_t>(bytes[at + k]) << (8 * k);
            at += 4;
            float v;
            std::memcpy(&v, &raw, 4);
            if (!std::isfinite(v))
                return PastedSetup::Damaged;
            char number[48];
            snprintf(number, sizeof number, "%f", v);
            value = number;
        }
        // (Never a personal line, whatever a code says.)
        if (!IsPersonal(key) && key != "bike" && key.find_first_of("\r\n=") == std::string::npos &&
            value.find_first_of("\r\n") == std::string::npos)
            result.tuning.emplace_back(key, value);
    }
    if (result.bike.empty() || result.bike.find_first_of("\r\n") != std::string::npos)
        return PastedSetup::Damaged;
    return PastedSetup::Saved;  // (read whole)
}

// The shared setup written as a new setup: the bike, your own lines (from own: a setup file of yours),
// then its tuning - under wantedName (else "shared"), a number added while that's taken.
PastedSetup WriteShared(const DecodedSetup& d, const std::wstring& own, const std::string& wantedName)
{
    PastedSetup result;
    result.bike = d.bike;
    bool ok = false;
    const auto lines = ReadSetupFile(own, ok);
    std::vector<std::pair<std::string, std::string>> personal;
    for (const auto& kv : lines)
        if (IsPersonal(kv.first))
            personal.push_back(kv);
    if (!ok || personal.empty()) {
        result.status = PastedSetup::NoOwnSetup;
        return result;
    }
    const std::filesystem::path folder(Folder());
    // (The name typed; else the setup's own, as its owner called it; else "shared".)
    const std::string base = ValidName(wantedName) ? wantedName : ValidName(d.name) ? d.name : std::string("shared");
    std::string name = base;
    for (int k = 2; k < 1000; ++k) {
        std::error_code e3;
        if (!std::filesystem::exists(folder / std::filesystem::u8path(name), e3))
            break;
        name = base + "_" + std::to_string(k);
    }
    // Written as the game writes them: the bike, your own lines, then the tuning.
    const std::wstring path = (folder / std::filesystem::u8path(name)).wstring();
    FILE* f = nullptr;
#ifdef _WIN32
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0)
        f = nullptr;
#else
    f = fopen(std::string(path.begin(), path.end()).c_str(), "wb");
#endif
    if (!f) {
        result.status = PastedSetup::Failed;
        Log("setups: couldn't write the pasted setup %ls", path.c_str());
        return result;
    }
    std::string text = "bike=" + d.bike + "\n";
    for (const auto& kv : personal)
        text += kv.first + "=" + kv.second + "\n";
    for (const auto& kv : d.tuning)
        text += kv.first + "=" + kv.second + "\n";
    const bool written = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    if (!written) {
        result.status = PastedSetup::Failed;
        return result;
    }
    result.name = name;
    result.status = PastedSetup::Saved;
    g_listedAt = -10.0;  // (in the list at once)
    Log("setups: a shared setup for %s saved as \"%s\" (%zu tuning lines; your own name, gear, controls and settings from %ls)",
        d.bike.c_str(), name.c_str(), d.tuning.size(), std::filesystem::path(own).filename().wstring().c_str());
    return result;
}

// A paste waiting for your own setup to be written (by the game, from your bike as it is: kOwnSetup).
struct PendingPaste {
    bool         active = false;
    DecodedSetup setup;
    std::string  wantedName;
};
PendingPaste g_paste;

// The game wrote your own setup (or didn't): the shared one written from it, the temporary file gone.
void FinishPaste(bool written, const std::string& answer)
{
    if (!g_paste.active)
        return;
    g_paste.active = false;
    const std::filesystem::path own = std::filesystem::path(Folder()) / kOwnSetup;
    if (!written) {
        snprintf(g_status, sizeof g_status, "COULDN'T READ YOUR OWN SETTINGS FROM YOUR BIKE%s%s", answer.empty() ? "" : " - THE GAME SAYS: ",
                 Upper(answer).c_str());
        return;
    }
    const PastedSetup r = WriteShared(g_paste.setup, own.wstring(), g_paste.wantedName);
    std::error_code error;
    std::filesystem::remove(own, error);
    g_listedAt = -10.0;
    switch (r.status) {
    case PastedSetup::Saved:
        snprintf(g_status, sizeof g_status, "%s SETUP SAVED AS \"%s\" - LOAD IT", Upper(r.bike).c_str(), r.name.c_str());
        break;
    case PastedSetup::NoOwnSetup:
        snprintf(g_status, sizeof g_status, "COULDN'T READ YOUR OWN SETTINGS FROM YOUR BIKE");
        break;
    default:
        snprintf(g_status, sizeof g_status, "COULD NOT WRITE THE SETUP FILE");
        break;
    }
}
}  // namespace

PastedSetup PasteSetupCode(const char* text, const std::string& wantedName, double now)
{
    PastedSetup result;
    DecodedSetup d;
    result.status = DecodeSetup(text, d);
    result.bike = d.bike;
    if (result.status != PastedSetup::Saved)
        return result;
    // Your own lines from your bike as it is now: the game writes it out (/writesetup) first.
    if (Busy() || !Send(kOwnSetupName, true, now)) {
        result.status = PastedSetup::Failed;
        return result;
    }
    g_paste.active = true;
    g_paste.setup = d;
    g_paste.wantedName = wantedName;
    snprintf(g_status, sizeof g_status, "READING YOUR OWN SETTINGS FROM YOUR BIKE...");
    Log("setups: pasting a shared setup for %s: your own settings read from your bike first", d.bike.c_str());
    result.status = PastedSetup::Reading;
    return result;
}

PastedSetup PasteSetupCodeFrom(const char* text, const std::string& wantedName, const std::wstring& ownFile)
{
    PastedSetup result;
    DecodedSetup d;
    result.status = DecodeSetup(text, d);
    result.bike = d.bike;
    if (result.status != PastedSetup::Saved)
        return result;
    return WriteShared(d, ownFile, wantedName);
}

// A setup's file renamed or deleted (yours, in your setups folder: no console needed). Not one the
// game is loading or writing this moment.
const char* Rename(const std::string& from, const std::string& to)
{
    if (Busy() && (g_name == from || g_name == to))
        return "WAIT A MOMENT: THE GAME IS STILL ON THAT SETUP";
    if (!ValidName(to))
        return "A NAME IS 1 TO 40 LETTERS, NUMBERS, - _ OR .";
    if (to == from)
        return nullptr;
    const std::filesystem::path folder(Folder());
    std::error_code error;
    // (Another setup's name: never written over. Only the case changed: the same file, renamed.)
    if (std::filesystem::exists(folder / std::filesystem::u8path(to), error) && Upper(to) != Upper(from))
        return "THERE'S A SETUP WITH THAT NAME ALREADY";
    std::filesystem::rename(folder / std::filesystem::u8path(from), folder / std::filesystem::u8path(to), error);
    if (error) {
        Log("setups: couldn't rename \"%s\" to \"%s\" (%s)", from.c_str(), to.c_str(), error.message().c_str());
        return "COULDN'T RENAME IT";
    }
    if (g_current == from)
        g_current = to;
    g_listedAt = -10.0;
    Log("setups: \"%s\" renamed \"%s\"", from.c_str(), to.c_str());
    return nullptr;
}

const char* Delete(const std::string& name)
{
    if (Busy() && g_name == name)
        return "WAIT A MOMENT: THE GAME IS STILL ON THAT SETUP";
    if (name.empty() || name.find_first_of("/\\") != std::string::npos || name == "." || name == "..")
        return "COULDN'T DELETE IT";
    std::error_code error;
    if (!std::filesystem::remove(std::filesystem::path(Folder()) / std::filesystem::u8path(name), error) || error) {
        Log("setups: couldn't delete \"%s\" (%s)", name.c_str(), error ? error.message().c_str() : "not there");
        return "COULDN'T DELETE IT";
    }
    if (g_current == name)
        g_current.clear();
    g_listedAt = -10.0;
    Log("setups: \"%s\" deleted", name.c_str());
    return nullptr;
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
            if (g_paste.active && g_name == kOwnSetupName) {
                FinishPaste(done, done ? std::string() : answer);  // (a paste: its shared setup written now)
            } else if (done) {
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
            g_paste.active = false;
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
