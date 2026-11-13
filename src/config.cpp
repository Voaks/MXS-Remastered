// config.cpp
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "config.h"
#include "log.h"
#include "mathlib.h"

namespace mxcam {
namespace {

Config  g_config;
wchar_t g_path[1024] = L"";

// Presets: five whole sets of the settings (CopyTunables). The live settings (g_config, kept in
// the ini's usual sections) are the active preset's; the others are kept here, and in the ini in
// sections of their own ([Preset1] .. [Preset5]) with their keys as "Section.Key".
Config  g_presets[kPresetCount];
int     g_activePreset = 0;
std::string g_presetNames[kPresetCount];  // (see PresetName: never part of a preset's settings)
const wchar_t* g_presetSection = nullptr;  // while reading / writing a preset's section
// While reading a preset code (LoadPresetCode): its values by "Section.Key", read instead of the ini.
const std::vector<std::pair<std::wstring, std::wstring>>* g_codeValues = nullptr;

bool ReadRaw(const wchar_t* section, const wchar_t* key, wchar_t* buffer, DWORD size)
{
    if (g_codeValues) {
        wchar_t full[128];
        swprintf_s(full, L"%s.%s", section, key);
        buffer[0] = L'\0';
        for (const auto& v : *g_codeValues)
            if (v.first == full) {
                wcsncpy_s(buffer, size, v.second.c_str(), _TRUNCATE);
                break;
            }
    } else if (g_presetSection) {
        wchar_t full[128];
        swprintf_s(full, L"%s.%s", section, key);
        GetPrivateProfileStringW(g_presetSection, full, L"", buffer, size, g_path);
    } else {
        GetPrivateProfileStringW(section, key, L"", buffer, size, g_path);
    }
    return buffer[0] != L'\0';
}

const wchar_t* PresetSection(int index)
{
    static const wchar_t* const kSections[kPresetCount] = { L"Preset1", L"Preset2", L"Preset3", L"Preset4", L"Preset5" };
    return kSections[index];
}

double ReadDouble(const wchar_t* section, const wchar_t* key, double fallback)
{
    wchar_t buffer[64];
    if (!ReadRaw(section, key, buffer, _countof(buffer)))
        return fallback;
    wchar_t* end = nullptr;
    const double value = wcstod(buffer, &end);
    return end != buffer ? value : fallback;
}

int ReadInt(const wchar_t* section, const wchar_t* key, int fallback)
{
    wchar_t buffer[64];
    if (!ReadRaw(section, key, buffer, _countof(buffer)))
        return fallback;
    wchar_t* end = nullptr;
    const long value = wcstol(buffer, &end, 0);  // base 0: accepts 120 or 0x78
    return end != buffer ? static_cast<int>(value) : fallback;
}

void ReadString(const wchar_t* section, const wchar_t* key, char (&value)[64])
{
    wchar_t buffer[64];
    if (!ReadRaw(section, key, buffer, _countof(buffer)))
        return;
    // Keep only the first word, so a trailing "; comment" can't sneak into the name.
    wchar_t* end = buffer;
    while (*end && *end != L' ' && *end != L'\t' && *end != L';')
        ++end;
    *end = L'\0';
    if (!WideCharToMultiByte(CP_UTF8, 0, buffer, -1, value, sizeof value, nullptr, nullptr))
        value[0] = '\0';
}

void ReadVector(const wchar_t* section, const wchar_t* key, double (&value)[3])
{
    wchar_t buffer[128];
    double x, y, z;
    if (!ReadRaw(section, key, buffer, _countof(buffer)) ||
        swscanf_s(buffer, L"%lf , %lf , %lf", &x, &y, &z) != 3)
        return;
    const double length = std::sqrt(x * x + y * y + z * z);
    if (length < 1e-9)
        return;
    value[0] = x / length;
    value[1] = y / length;
    value[2] = z / length;
}

double NowSeconds()
{
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return double(now.QuadPart) / double(frequency.QuadPart);
}

// Edits to the ini made in memory and written out in one go. WritePrivateProfileString reads and
// rewrites the whole file on every call: a save (~400 keys - the settings and the presets) took
// 1.4 s, the game frozen meanwhile (and the bike tracker took it for a pause). The same rules as
// Windows': sections and keys match regardless of case and surrounding spaces, comments and other
// keys stay as they are, a new key goes after the last line of its section, a new section at the
// end of the file. A UTF-16 file (which this mod never writes) is left to Windows, call by call.
class IniBatch {
public:
    explicit IniBatch(const wchar_t* path) : path_(path)
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"rb") != 0 || !f)
            return;  // (no file yet: it's made on Commit)
        std::string text;
        char buffer[8192];
        size_t n;
        while ((n = fread(buffer, 1, sizeof buffer, f)) > 0)
            text.append(buffer, n);
        fclose(f);
        if (text.size() >= 2 && static_cast<unsigned char>(text[0]) == 0xFF && static_cast<unsigned char>(text[1]) == 0xFE) {
            direct_ = true;
            return;
        }
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos)
                end = text.size();
            std::string line = text.substr(start, end - start);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines_.push_back(line);
            start = end + 1;
        }
    }

    // value nullptr removes the key; key nullptr removes the whole section.
    bool Set(const wchar_t* section, const wchar_t* key, const wchar_t* value)
    {
        if (direct_)
            return WritePrivateProfileStringW(section, key, value, path_) != FALSE;
        const std::string s = Trim(Narrow(section));
        size_t begin, end;
        const bool found = FindSection(s, begin, end);
        if (!key) {
            if (found) {
                lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(begin - 1), lines_.begin() + static_cast<std::ptrdiff_t>(end));
                changed_ = true;
            }
            return true;
        }
        const std::string k = Trim(Narrow(key));
        if (found) {
            for (size_t i = begin; i < end; ++i) {
                std::string name;
                if (KeyOf(lines_[i], name) && _stricmp(name.c_str(), k.c_str()) == 0) {
                    if (value)
                        lines_[i] = name + "=" + Narrow(value);  // (the key as the file spells it)
                    else
                        lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(i));
                    changed_ = true;
                    return true;
                }
            }
        }
        if (!value)
            return true;
        const std::string line = k + "=" + Narrow(value);
        if (found) {
            size_t at = begin;  // after the section's last key (a comment after it may be the next section's)
            for (size_t i = begin; i < end; ++i) {
                std::string name;
                if (KeyOf(lines_[i], name))
                    at = i + 1;
            }
            lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(at), line);
        } else {
            lines_.push_back("[" + s + "]");
            lines_.push_back(line);
        }
        changed_ = true;
        return true;
    }

    // Writes the file (if anything changed): to a temporary file first, then over the ini, so a
    // failure part way can't leave half an ini.
    bool Commit()
    {
        if (direct_ || !changed_)
            return true;
        std::string text;
        for (const std::string& line : lines_) {
            text += line;
            text += "\r\n";
        }
        wchar_t temp[1100];
        swprintf_s(temp, L"%s.saving", path_);
        FILE* f = nullptr;
        if (_wfopen_s(&f, temp, L"wb") != 0 || !f)
            return false;
        const bool written = fwrite(text.data(), 1, text.size(), f) == text.size();
        const bool closed = fclose(f) == 0;
        if (!written || !closed || !MoveFileExW(temp, path_, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temp);
            return false;
        }
        changed_ = false;
        return true;
    }

private:
    static std::string Narrow(const wchar_t* w)
    {
        char buffer[512] = "";
        WideCharToMultiByte(CP_ACP, 0, w, -1, buffer, sizeof buffer, nullptr, nullptr);
        return buffer;
    }
    static std::string Trim(const std::string& s)
    {
        const size_t a = s.find_first_not_of(" \t");
        if (a == std::string::npos)
            return std::string();
        const size_t b = s.find_last_not_of(" \t");
        return s.substr(a, b - a + 1);
    }
    // A "[name]" line: its name.
    static bool SectionOf(const std::string& line, std::string& name)
    {
        const std::string t = Trim(line);
        if (t.empty() || t[0] != '[')
            return false;
        const size_t close = t.find(']');
        if (close == std::string::npos)
            return false;
        name = Trim(t.substr(1, close - 1));
        return true;
    }
    // A "key=value" line (not a comment): its key.
    static bool KeyOf(const std::string& line, std::string& name)
    {
        const std::string t = Trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#' || t[0] == '[')
            return false;
        const size_t equals = t.find('=');
        if (equals == std::string::npos)
            return false;
        name = Trim(t.substr(0, equals));
        return true;
    }
    // The section's lines: [begin, end), its header at begin - 1.
    bool FindSection(const std::string& section, size_t& begin, size_t& end) const
    {
        for (size_t i = 0; i < lines_.size(); ++i) {
            std::string name;
            if (SectionOf(lines_[i], name) && _stricmp(name.c_str(), section.c_str()) == 0) {
                begin = i + 1;
                end = begin;
                while (end < lines_.size() && !SectionOf(lines_[end], name))
                    ++end;
                return true;
            }
        }
        return false;
    }

    const wchar_t*           path_;
    std::vector<std::string> lines_;
    bool                     direct_  = false;
    bool                     changed_ = false;
};

// Every setting the presets and DEFAULTS cover (CopyTunables), written into the usual sections,
// or into a preset's own (keys "Section.Key").
struct TunableValue {
    const wchar_t* section;
    const wchar_t* key;
    double         value;
};
std::vector<TunableValue> Tunables(const Config& c)
{
    return {
        { L"Camera",    L"CustomCamera",         c.customCamera ? 1.0 : 0.0 },
        { L"Camera",    L"CameraAngleDeg",       c.cameraAngleDeg },
        { L"Camera",    L"HeadForward",          c.headForward },
        { L"Camera",    L"HeadHeight",           c.headHeight },
        { L"Camera",    L"HideOwnBody",          c.hideOwnBody ? 1.0 : 0.0 },
        { L"Smoothing", L"RotationResponse",     c.rotationResponse },
        { L"Smoothing", L"MaxLagDeg",            c.maxLagDeg },
        { L"Leveling",  L"HorizonLock",          c.horizonLock },
        { L"Leveling",  L"PitchLock",            c.pitchLock },
        { L"Leveling",  L"MaxRollCorrectionDeg", c.maxRollCorrectionDeg },
        { L"Leveling",  L"Steadiness",           c.steadiness },
        { L"Air",       L"AirLooseness",         c.airLooseness },
        { L"Air",       L"WhipLock",             c.whipLock },
        { L"Look",      L"ControllerLook",       c.controllerLook ? 1.0 : 0.0 },
        { L"Look",      L"LookSideDeg",          c.lookSideDeg },
        { L"Look",      L"LookTurnSpeed",        c.lookTurnSpeed },
        { L"Look",      L"ApexLook",             c.apexLook },
        { L"Look",      L"ApexSpeed",            c.apexSpeed },
        { L"Look",      L"ApexAnticipation",     c.apexAnticipation },
        { L"Look",      L"LookSideForward",      c.lookSideForward },
        { L"Look",      L"LookSideHeight",       c.lookSideHeight },
        { L"Look",      L"LookBackForward",      c.lookBackForward },
        { L"Look",      L"LookBackHeight",       c.lookBackHeight },
        { L"Neck",      L"VerticalResponse",     c.verticalResponse },
        { L"Neck",      L"MaxVerticalLag",       c.maxVerticalLag },
        { L"Neck",      L"CompressionPerG",      c.compressionPerG },
        { L"Neck",      L"SpringHz",             c.springHz },
        { L"Neck",      L"DampingRatio",         c.dampingRatio },
        { L"Neck",      L"MaxCompression",       c.maxCompression },
        { L"Neck",      L"MaxExtension",         c.maxExtension },
        { L"Neck",      L"NodDegPerMeter",       c.nodDegPerMeter },
        { L"Neck",      L"AccelFilterHz",        c.accelFilterHz },
        { L"Replay",    L"Camera",               static_cast<double>(c.replayCamera) },
        { L"Replay",    L"Distance",             c.replayDistance },
        { L"Replay",    L"Height",               c.replayHeight },
        { L"Replay",    L"PitchDeg",             c.replayPitchDeg },
        { L"Replay",    L"HelmetHeight",         c.helmetHeight },
        { L"Replay",    L"HelmetForward",        c.helmetForward },
        { L"Replay",    L"HelmetTiltDeg",        c.helmetTiltDeg },
        { L"Replay",    L"HelmetMount",          static_cast<double>(c.helmetMount) },
        { L"Replay",    L"SideHeight",           c.sideHeight },
        { L"Replay",    L"SideForward",          c.sideForward },
        { L"Replay",    L"SideTiltDeg",          c.sideTiltDeg },
        { L"Replay",    L"SideOut",              c.sideOut },
        { L"Replay",    L"ChestHeight",          c.chestHeight },
        { L"Replay",    L"ChestForward",         c.chestForward },
        { L"Replay",    L"ChestTiltDeg",         c.chestTiltDeg },
        { L"Replay",    L"HelmetSmoothing",      c.helmetSmoothing },
        { L"Replay",    L"HelmetLevel",          c.helmetLevel },
        { L"Graphics",  L"Shaders",              c.shaders ? 1.0 : 0.0 },
        { L"Graphics",  L"TimeOfDay",            c.timeOfDay },
        { L"Graphics",  L"MoveSun",              c.moveSun ? 1.0 : 0.0 },
        { L"Graphics",  L"NightBrightness",      c.nightBrightness },
        { L"Graphics",  L"HideGameFlare",        c.hideGameFlare ? 1.0 : 0.0 },
        { L"Graphics",  L"Shadows",              c.shadows },
        { L"Graphics",  L"ShadowLength",         c.shadowLength },
        { L"Graphics",  L"ShadowMap",            c.shadowMap ? 1.0 : 0.0 },
        { L"Graphics",  L"ShadowDistanceM",      c.shadowDistance },
        { L"Graphics",  L"ShadowResolution",     static_cast<double>(c.shadowResolution) },
        { L"Graphics",  L"ShadowSoftness",       c.shadowSoftness },
        { L"Graphics",  L"Headlight",            c.headlight },
        { L"Graphics",  L"HeadlightBeamDeg",     c.headlightBeamDeg },
        { L"Graphics",  L"HeadlightAimDeg",      c.headlightAimDeg },
        { L"Graphics",  L"AmbientOcclusion",     c.ambientOcclusion },
        { L"Graphics",  L"SunRays",              c.sunRays },
        { L"Graphics",  L"Bloom",                c.bloom },
        { L"Graphics",  L"Exposure",             c.exposure },
        { L"Graphics",  L"Contrast",             c.contrast },
        { L"Graphics",  L"Saturation",           c.saturation },
        { L"Graphics",  L"Vignette",             c.vignette },
        { L"Graphics",  L"AntiAliasing",         static_cast<double>(c.antiAliasing) },
        { L"Graphics",  L"Sharpen",              c.sharpen },
        { L"Graphics",  L"Anisotropy",           c.anisotropy },
        { L"Graphics",  L"Trilinear",            c.trilinear ? 1.0 : 0.0 },
        { L"Graphics",  L"TextureLodBias",       c.textureLodBias },
        { L"Graphics",  L"DirtDetail",           c.dirtDetail },
        { L"Graphics",  L"DirtScale",            c.dirtScale },
        { L"Graphics",  L"TireMarks",            c.tireMarks },
        { L"Graphics",  L"MarkCacheSegs",        c.markCacheSegs },
        { L"Graphics",  L"MarkDepth",            c.markDepth },
        { L"Graphics",  L"MarksEveryBike",       c.marksEveryBike ? 1.0 : 0.0 },
        { L"Graphics",  L"HeadlightEveryBike",   c.headlightEveryBike ? 1.0 : 0.0 },
        { L"Graphics",  L"HeadlightColor",       static_cast<double>(c.headlightColor) },
        { L"Graphics",  L"TireDirt",             c.tireDirt },
        { L"Graphics",  L"MotionBlur",           c.motionBlur },
        { L"Graphics",  L"DepthOfField",         c.depthOfField },
        { L"Graphics",  L"DofFocus",             c.dofFocus },
        { L"Weather",   L"Snow",                 c.snow ? 1.0 : 0.0 },
        { L"Weather",   L"Snowfall",             c.snowfall },
        { L"Weather",   L"Wind",                 c.snowWind },
        { L"Other",     L"HideHud",              c.hideHud ? 1.0 : 0.0 },
        { L"Other",     L"HideCursor",           c.hideCursor ? 1.0 : 0.0 },
        { L"Other",     L"DabIndicator",         c.dabIndicator ? 1.0 : 0.0 },
        { L"Other",     L"BrakeSqueal",          c.brakeSqueal },
        { L"Other",     L"DabSticky",            static_cast<double>(c.dabSticky) },
        { L"Other",     L"FadeMenu",             c.fadeMenu ? 1.0 : 0.0 },
    };
}

bool WriteTunables(IniBatch& ini, const Config& c, const wchar_t* presetSection)
{
    bool ok = true;
    for (const TunableValue& v : Tunables(c)) {
        wchar_t text[32];
        swprintf_s(text, L"%.6g", v.value);
        if (presetSection) {
            wchar_t full[128];
            swprintf_s(full, L"%s.%s", v.section, v.key);
            ok = ini.Set(presetSection, full, text) && ok;
        } else {
            ok = ini.Set(v.section, v.key, text) && ok;
        }
    }
    return ok;
}

}  // namespace

const Config& GetConfig() { return g_config; }

Config& EditConfig() { return g_config; }

void SetEnabled(bool enabled) { g_config.enabled = enabled; }

bool SaveConfig()
{
    if (!g_path[0])
        return false;

    const double started = NowSeconds();
    IniBatch ini(g_path);
    bool ok = ini.Set(L"General", L"Enabled", g_config.enabled ? L"1" : L"0");
    // The keybinds (the KEYBINDS tab).
    const struct { const wchar_t* section; const wchar_t* key; int vk; } keys[] = {
        { L"General",  L"MenuKey",   g_config.menuKey },
        { L"General",  L"ToggleKey", g_config.toggleKey },
        { L"Graphics", L"ToggleKey", g_config.shaderToggleKey },
    };
    for (const auto& k : keys) {
        wchar_t text[16];
        swprintf_s(text, L"0x%02X", k.vk);
        ok = ini.Set(k.section, k.key, text) && ok;
    }
    // The controller's look binds (by name: DPAD_LEFT, RS_X-, PADDLE1...).
    const struct { const wchar_t* key; int control; } looks[] = {
        { L"LookLeftControl",  g_config.lookLeftControl },
        { L"LookRightControl", g_config.lookRightControl },
        { L"LookBackControl",  g_config.lookBackControl },
    };
    for (const auto& l : looks) {
        wchar_t text[32];
        swprintf_s(text, L"%hs", gamepad::ControlKey(l.control));
        ok = ini.Set(L"Look", l.key, text) && ok;
    }
    // A display choice, shared by the presets.
    ok = ini.Set(L"Other", L"FullscreenWindow", g_config.fullscreenWindow ? L"1" : L"0") && ok;
    ok = WriteTunables(ini, g_config, nullptr) && ok;
    // The presets: which one is live (its settings are the ones above), and the others' own.
    wchar_t active[8];
    swprintf_s(active, L"%d", g_activePreset + 1);
    ok = ini.Set(L"Presets", L"Active", active) && ok;
    for (int i = 0; i < kPresetCount; ++i) {
        wchar_t key[16];
        swprintf_s(key, L"Name%d", i + 1);
        if (g_presetNames[i].empty()) {
            ini.Set(L"Presets", key, nullptr);
            continue;
        }
        // In quotes, so spaces at either end (and quotes of its own) read back as they are.
        wchar_t value[kPresetNameMax + 8];
        swprintf_s(value, L"\"%hs\"", g_presetNames[i].c_str());
        ok = ini.Set(L"Presets", key, value) && ok;
    }
    for (int i = 0; i < kPresetCount; ++i) {
        if (i == g_activePreset) {
            ini.Set(PresetSection(i), nullptr, nullptr);  // it's the usual sections
            continue;
        }
        Config preset = g_config;
        CopyTunables(g_presets[i], preset);
        ok = WriteTunables(ini, preset, PresetSection(i)) && ok;
    }
    // Renamed to HorizonLock; drop the old key so the file can't disagree with itself.
    ini.Set(L"Leveling", L"LeanCompensation", nullptr);
    ini.Set(L"Replay", L"ChaseCam", nullptr);  // now Camera
    // Keep game angle is gone: the custom camera uses its look-down angle, the game camera the game's.
    ini.Set(L"Camera", L"KeepGameAngle", nullptr);
    for (int i = 0; i < kPresetCount; ++i)
        ini.Set(PresetSection(i), L"Camera.KeepGameAngle", nullptr);
    ok = ini.Commit() && ok;
    Log("config: %s %ls (%.0f ms)", ok ? "saved" : "FAILED to save", g_path, (NowSeconds() - started) * 1000.0);
    return ok;
}

void CopyTunables(const Config& from, Config& to)
{
    to.customCamera         = from.customCamera;
    to.cameraAngleDeg       = from.cameraAngleDeg;
    to.headForward          = from.headForward;
    to.headHeight           = from.headHeight;
    to.hideOwnBody          = from.hideOwnBody;
    to.rotationResponse     = from.rotationResponse;
    to.maxLagDeg            = from.maxLagDeg;
    to.horizonLock          = from.horizonLock;
    to.pitchLock            = from.pitchLock;
    to.maxRollCorrectionDeg = from.maxRollCorrectionDeg;
    to.steadiness           = from.steadiness;
    to.airLooseness         = from.airLooseness;
    to.whipLock             = from.whipLock;
    to.controllerLook       = from.controllerLook;
    to.lookSideDeg          = from.lookSideDeg;
    to.lookTurnSpeed        = from.lookTurnSpeed;
    to.apexLook             = from.apexLook;
    to.apexSpeed            = from.apexSpeed;
    to.apexAnticipation     = from.apexAnticipation;
    to.lookSideForward      = from.lookSideForward;
    to.lookSideHeight       = from.lookSideHeight;
    to.lookBackForward      = from.lookBackForward;
    to.lookBackHeight       = from.lookBackHeight;
    to.verticalResponse     = from.verticalResponse;
    to.maxVerticalLag       = from.maxVerticalLag;
    to.compressionPerG      = from.compressionPerG;
    to.springHz             = from.springHz;
    to.dampingRatio         = from.dampingRatio;
    to.maxCompression       = from.maxCompression;
    to.maxExtension         = from.maxExtension;
    to.nodDegPerMeter       = from.nodDegPerMeter;
    to.accelFilterHz        = from.accelFilterHz;
    to.replayCamera         = from.replayCamera;
    to.replayDistance       = from.replayDistance;
    to.replayHeight         = from.replayHeight;
    to.replayPitchDeg       = from.replayPitchDeg;
    to.helmetHeight         = from.helmetHeight;
    to.helmetForward        = from.helmetForward;
    to.helmetTiltDeg        = from.helmetTiltDeg;
    to.helmetMount          = from.helmetMount;
    to.sideHeight           = from.sideHeight;
    to.sideForward          = from.sideForward;
    to.sideTiltDeg          = from.sideTiltDeg;
    to.sideOut              = from.sideOut;
    to.chestHeight          = from.chestHeight;
    to.chestForward         = from.chestForward;
    to.chestTiltDeg         = from.chestTiltDeg;
    to.helmetSmoothing      = from.helmetSmoothing;
    to.helmetLevel          = from.helmetLevel;
    to.shaders              = from.shaders;
    to.timeOfDay            = from.timeOfDay;
    to.moveSun              = from.moveSun;
    to.nightBrightness      = from.nightBrightness;
    to.hideGameFlare        = from.hideGameFlare;
    to.shadows              = from.shadows;
    to.shadowLength         = from.shadowLength;
    to.shadowMap            = from.shadowMap;
    to.shadowDistance       = from.shadowDistance;
    to.shadowResolution     = from.shadowResolution;
    to.shadowSoftness       = from.shadowSoftness;
    to.shadowMapSize        = from.shadowMapSize;
    to.headlight            = from.headlight;
    to.headlightBeamDeg     = from.headlightBeamDeg;
    to.headlightAimDeg      = from.headlightAimDeg;
    to.ambientOcclusion     = from.ambientOcclusion;
    to.sunRays              = from.sunRays;
    to.bloom                = from.bloom;
    to.exposure             = from.exposure;
    to.contrast             = from.contrast;
    to.saturation           = from.saturation;
    to.vignette             = from.vignette;
    to.antiAliasing         = from.antiAliasing;
    to.sharpen              = from.sharpen;
    to.anisotropy           = from.anisotropy;
    to.trilinear            = from.trilinear;
    to.textureLodBias       = from.textureLodBias;
    to.dirtDetail           = from.dirtDetail;
    to.dirtScale            = from.dirtScale;
    to.tireMarks            = from.tireMarks;
    to.markCacheSegs        = from.markCacheSegs;
    to.markDepth            = from.markDepth;
    to.marksEveryBike       = from.marksEveryBike;
    to.headlightEveryBike   = from.headlightEveryBike;
    to.headlightColor       = from.headlightColor;
    to.snow                 = from.snow;
    to.snowfall             = from.snowfall;
    to.snowWind             = from.snowWind;
    to.tireDirt             = from.tireDirt;
    to.motionBlur           = from.motionBlur;
    to.depthOfField         = from.depthOfField;
    to.dofFocus             = from.dofFocus;
    to.hideHud              = from.hideHud;
    to.hideCursor           = from.hideCursor;
    to.dabIndicator         = from.dabIndicator;
    to.brakeSqueal          = from.brakeSqueal;
    to.dabSticky            = from.dabSticky;
    to.fadeMenu             = from.fadeMenu;
}

void ResetTunables()
{
    CopyTunables(Config{}, g_config);
}

int ActivePreset() { return g_activePreset; }

namespace {
std::string CleanPresetName(const std::string& name)
{
    std::string clean;
    for (const char ch : name) {
        if (ch >= 32 && ch <= 126 && static_cast<int>(clean.size()) < kPresetNameMax)
            clean += ch;
    }
    const size_t first = clean.find_first_not_of(' ');
    if (first == std::string::npos)
        return std::string();
    return clean.substr(first, clean.find_last_not_of(' ') - first + 1);
}
}  // namespace

std::string PresetName(int index)
{
    return index >= 0 && index < kPresetCount ? g_presetNames[index] : std::string();
}

std::string PresetLabel(int index)
{
    const std::string name = PresetName(index);
    return name.empty() ? std::to_string(index + 1) : name;
}

void SetPresetName(int index, const std::string& name)
{
    if (index < 0 || index >= kPresetCount)
        return;
    g_presetNames[index] = CleanPresetName(name);
    Log("config: preset %d named '%s'", index + 1, g_presetNames[index].c_str());
}

void SwitchPreset(int index)
{
    if (index < 0 || index >= kPresetCount || index == g_activePreset)
        return;
    CopyTunables(g_config, g_presets[g_activePreset]);  // the live settings stay with their preset
    CopyTunables(g_presets[index], g_config);
    g_activePreset = index;
    Log("config: preset %d", index + 1);
}

void LoadConfig(const wchar_t* iniPath)
{
    wcscpy_s(g_path, iniPath);
    ReloadConfig();
}

bool ReadLearned(const char* key, double value[3])
{
    if (!g_path[0])
        return false;
    wchar_t wkey[64] = L"", buffer[128] = L"";
    MultiByteToWideChar(CP_UTF8, 0, key, -1, wkey, 64);
    GetPrivateProfileStringW(L"Learned", wkey, L"", buffer, 128, g_path);
    double v[3];
    if (swscanf_s(buffer, L"%lf,%lf,%lf", &v[0], &v[1], &v[2]) != 3)
        return false;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(v[i]) || std::fabs(v[i]) > 100.0)
            return false;
        value[i] = v[i];
    }
    return true;
}

void WriteLearned(const char* key, const double value[3])
{
    if (!g_path[0])
        return;
    wchar_t wkey[64] = L"", text[128] = L"";
    MultiByteToWideChar(CP_UTF8, 0, key, -1, wkey, 64);
    swprintf_s(text, L"%.3f,%.3f,%.3f", value[0], value[1], value[2]);
    WritePrivateProfileStringW(L"Learned", wkey, text, g_path);
}

void ForgetLearned(const char* key)
{
    if (!g_path[0])
        return;
    wchar_t wkey[64] = L"";
    MultiByteToWideChar(CP_UTF8, 0, key, -1, wkey, 64);
    WritePrivateProfileStringW(L"Learned", wkey, nullptr, g_path);
}

namespace {

// Everything in the ini (or, with g_presetSection set, a preset's section), over base: what's
// missing keeps base's value. Every value is kept to a range the physics can't blow up with.
Config ParseConfig(const Config& base)
{
    Config c = base;
    c.enabled   = ReadInt(L"General", L"Enabled", c.enabled) != 0;
    c.toggleKey = ReadInt(L"General", L"ToggleKey", c.toggleKey);
    c.reloadKey = ReadInt(L"General", L"ReloadKey", c.reloadKey);
    c.dumpKey   = ReadInt(L"General", L"DumpKey", c.dumpKey);
    c.menuKey   = ReadInt(L"General", L"MenuKey", c.menuKey);
    c.traceKey  = ReadInt(L"General", L"TraceKey", c.traceKey);
    // The controller's look binds: a name (NONE unbinds it; anything unknown keeps the default).
    const struct { const wchar_t* key; int Config::* field; } looks[] = {
        { L"LookLeftControl",  &Config::lookLeftControl },
        { L"LookRightControl", &Config::lookRightControl },
        { L"LookBackControl",  &Config::lookBackControl },
    };
    for (const auto& l : looks) {
        char name[64] = "";
        ReadString(L"Look", l.key, name);
        if (name[0]) {
            const gamepad::Control control = gamepad::ParseControl(name);
            if (control != gamepad::kNone || _stricmp(name, "NONE") == 0)
                c.*l.field = control;
        }
    }

    ReadString(L"Detection", L"ModelviewUniform", c.modelviewUniform);
    ReadString(L"Detection", L"CameraProgramUniform", c.cameraProgramUniform);
    ReadString(L"Detection", L"LightUniform", c.lightUniform);
    ReadString(L"Detection", L"EnvMatrixUniform", c.envUniform);
    c.viewMatrixIndex  = ReadInt(L"Detection", L"ViewMatrixIndex", c.viewMatrixIndex);
    ReadVector(L"Detection", L"WorldUp", c.worldUp);
    c.snapAngleDeg     = ReadDouble(L"Detection", L"SnapAngleDeg", c.snapAngleDeg);
    c.teleportDistance = ReadDouble(L"Detection", L"TeleportDistance", c.teleportDistance);
    c.bikeSearchRadius = ReadDouble(L"Detection", L"BikeSearchRadius", c.bikeSearchRadius);

    c.customCamera   = ReadInt(L"Camera", L"CustomCamera", c.customCamera) != 0;
    c.cameraAngleDeg = ReadDouble(L"Camera", L"CameraAngleDeg", c.cameraAngleDeg);
    c.headForward    = ReadDouble(L"Camera", L"HeadForward", c.headForward);
    c.headHeight     = ReadDouble(L"Camera", L"HeadHeight", c.headHeight);
    c.hideOwnBody    = ReadInt(L"Camera", L"HideOwnBody", c.hideOwnBody) != 0;

    c.rotationResponse = ReadDouble(L"Smoothing", L"RotationResponse", c.rotationResponse);
    c.maxLagDeg        = ReadDouble(L"Smoothing", L"MaxLagDeg", c.maxLagDeg);

    // HorizonLock was called LeanCompensation in earlier versions.
    c.horizonLock          = ReadDouble(L"Leveling", L"LeanCompensation", c.horizonLock);
    c.horizonLock          = ReadDouble(L"Leveling", L"HorizonLock", c.horizonLock);
    c.pitchLock            = ReadDouble(L"Leveling", L"PitchLock", c.pitchLock);
    c.maxRollCorrectionDeg = ReadDouble(L"Leveling", L"MaxRollCorrectionDeg", c.maxRollCorrectionDeg);
    c.steadiness           = ReadDouble(L"Leveling", L"Steadiness", c.steadiness);

    c.airLooseness     = ReadDouble(L"Air", L"AirLooseness", c.airLooseness);
    c.whipLock         = ReadDouble(L"Air", L"WhipLock", c.whipLock);

    c.controllerLook  = ReadInt(L"Look", L"ControllerLook", c.controllerLook) != 0;
    c.lookSideDeg     = ReadDouble(L"Look", L"LookSideDeg", c.lookSideDeg);
    c.lookTurnSpeed   = ReadDouble(L"Look", L"LookTurnSpeed", c.lookTurnSpeed);
    c.apexLook        = ReadDouble(L"Look", L"ApexLook", c.apexLook);
    c.apexSpeed       = ReadDouble(L"Look", L"ApexSpeed", c.apexSpeed);
    c.apexAnticipation = ReadDouble(L"Look", L"ApexAnticipation", c.apexAnticipation);
    c.lookSideForward = ReadDouble(L"Look", L"LookSideForward", c.lookSideForward);
    c.lookSideHeight  = ReadDouble(L"Look", L"LookSideHeight", c.lookSideHeight);
    c.lookBackForward = ReadDouble(L"Look", L"LookBackForward", c.lookBackForward);
    c.lookBackHeight  = ReadDouble(L"Look", L"LookBackHeight", c.lookBackHeight);

    c.verticalResponse = ReadDouble(L"Neck", L"VerticalResponse", c.verticalResponse);
    c.maxVerticalLag   = ReadDouble(L"Neck", L"MaxVerticalLag", c.maxVerticalLag);
    c.compressionPerG  = ReadDouble(L"Neck", L"CompressionPerG", c.compressionPerG);
    c.springHz         = ReadDouble(L"Neck", L"SpringHz", c.springHz);
    c.dampingRatio     = ReadDouble(L"Neck", L"DampingRatio", c.dampingRatio);
    c.maxCompression   = ReadDouble(L"Neck", L"MaxCompression", c.maxCompression);
    c.maxExtension     = ReadDouble(L"Neck", L"MaxExtension", c.maxExtension);
    c.nodDegPerMeter   = ReadDouble(L"Neck", L"NodDegPerMeter", c.nodDegPerMeter);
    c.accelFilterHz    = ReadDouble(L"Neck", L"AccelFilterHz", c.accelFilterHz);

    // Camera replaced ChaseCam (0 / 1), which older files still have.
    c.replayCamera   = ReadInt(L"Replay", L"Camera", ReadInt(L"Replay", L"ChaseCam", c.replayCamera) != 0 ? 1 : 0);
    c.replayDistance = ReadDouble(L"Replay", L"Distance", c.replayDistance);
    c.replayHeight   = ReadDouble(L"Replay", L"Height", c.replayHeight);
    c.replayPitchDeg = ReadDouble(L"Replay", L"PitchDeg", c.replayPitchDeg);
    c.helmetHeight    = ReadDouble(L"Replay", L"HelmetHeight", c.helmetHeight);
    c.helmetForward   = ReadDouble(L"Replay", L"HelmetForward", c.helmetForward);
    c.helmetTiltDeg   = ReadDouble(L"Replay", L"HelmetTiltDeg", c.helmetTiltDeg);
    c.helmetMount     = ReadInt(L"Replay", L"HelmetMount", c.helmetMount);
    c.sideHeight      = ReadDouble(L"Replay", L"SideHeight", c.sideHeight);
    c.sideForward     = ReadDouble(L"Replay", L"SideForward", c.sideForward);
    c.sideTiltDeg     = ReadDouble(L"Replay", L"SideTiltDeg", c.sideTiltDeg);
    c.sideOut         = ReadDouble(L"Replay", L"SideOut", c.sideOut);
    c.chestHeight     = ReadDouble(L"Replay", L"ChestHeight", c.chestHeight);
    c.chestForward    = ReadDouble(L"Replay", L"ChestForward", c.chestForward);
    c.chestTiltDeg    = ReadDouble(L"Replay", L"ChestTiltDeg", c.chestTiltDeg);
    c.helmetSmoothing = ReadDouble(L"Replay", L"HelmetSmoothing", c.helmetSmoothing);
    c.helmetLevel     = ReadDouble(L"Replay", L"HelmetLevel", c.helmetLevel);

    c.shaders          = ReadInt(L"Graphics", L"Shaders", c.shaders) != 0;
    c.shaderToggleKey  = ReadInt(L"Graphics", L"ToggleKey", c.shaderToggleKey);
    c.applyBeforeHud   = ReadInt(L"Graphics", L"ApplyBeforeHud", c.applyBeforeHud) != 0;
    c.timeOfDay        = ReadDouble(L"Graphics", L"TimeOfDay", c.timeOfDay);
    c.moveSun          = ReadInt(L"Graphics", L"MoveSun", c.moveSun) != 0;
    c.nightBrightness  = ReadDouble(L"Graphics", L"NightBrightness", c.nightBrightness);
    c.hideGameFlare    = ReadInt(L"Graphics", L"HideGameFlare", c.hideGameFlare) != 0;
    c.invertLight      = ReadInt(L"Graphics", L"InvertLight", c.invertLight) != 0;
    c.shadows          = ReadDouble(L"Graphics", L"Shadows", c.shadows);
    c.shadowLength     = ReadDouble(L"Graphics", L"ShadowLength", c.shadowLength);
    c.shadowSoftness   = ReadDouble(L"Graphics", L"ShadowSoftness", c.shadowSoftness);
    c.shadowMap        = ReadInt(L"Graphics", L"ShadowMap", c.shadowMap) != 0;
    // Shadow distance in real metres (ShadowDistanceM). The old ShadowDistance was used in the
    // game's own unit, feet, though it said metres: read as that, so it covers what it did.
    {
        const double metres = ReadDouble(L"Graphics", L"ShadowDistanceM", -1.0);
        const double feet = ReadDouble(L"Graphics", L"ShadowDistance", -1.0);
        if (metres >= 0.0)
            c.shadowDistance = metres;
        else if (feet >= 0.0)
            c.shadowDistance = feet / 3.2808;
    }
    {
        const int resolution = ReadInt(L"Graphics", L"ShadowResolution", -1);
        const int size = ReadInt(L"Graphics", L"ShadowMapSize", -1);  // (the old way to set it)
        if (resolution >= 0)
            c.shadowResolution = resolution;
        else if (size > 0)
            c.shadowResolution = size >= 8192 ? 2 : size >= 4096 ? 1 : 0;
    }
    c.shadowBatch      = ReadInt(L"Graphics", L"ShadowBatch", c.shadowBatch) != 0;
    c.headlight        = ReadDouble(L"Graphics", L"Headlight", c.headlight);
    c.headlightBeamDeg = ReadDouble(L"Graphics", L"HeadlightBeamDeg", c.headlightBeamDeg);
    c.headlightForward = ReadDouble(L"Graphics", L"HeadlightForward", c.headlightForward);
    c.headlightHeight  = ReadDouble(L"Graphics", L"HeadlightHeight", c.headlightHeight);
    {
        // (Before HeadlightAimDeg: HeadlightPitchDeg, how far it was aimed down, its top edge 6
        // degrees under that.)
        const double aim = ReadDouble(L"Graphics", L"HeadlightAimDeg", 1e9);
        c.headlightAimDeg = aim < 1e8 ? aim : 6.0 - ReadDouble(L"Graphics", L"HeadlightPitchDeg", 6.0 - c.headlightAimDeg);
    }
    c.ambientOcclusion = ReadDouble(L"Graphics", L"AmbientOcclusion", c.ambientOcclusion);
    c.aoRadius         = ReadDouble(L"Graphics", L"AORadius", c.aoRadius);
    c.sunRays          = ReadDouble(L"Graphics", L"SunRays", c.sunRays);
    c.bloom            = ReadDouble(L"Graphics", L"Bloom", c.bloom);
    c.bloomThreshold   = ReadDouble(L"Graphics", L"BloomThreshold", c.bloomThreshold);
    c.exposure         = ReadDouble(L"Graphics", L"Exposure", c.exposure);
    c.contrast         = ReadDouble(L"Graphics", L"Contrast", c.contrast);
    c.saturation       = ReadDouble(L"Graphics", L"Saturation", c.saturation);
    c.vignette         = ReadDouble(L"Graphics", L"Vignette", c.vignette);
    c.antiAliasing     = ReadInt(L"Graphics", L"AntiAliasing", c.antiAliasing);
    c.sharpen          = ReadDouble(L"Graphics", L"Sharpen", c.sharpen);
    c.anisotropy       = ReadDouble(L"Graphics", L"Anisotropy", c.anisotropy);
    c.trilinear        = ReadInt(L"Graphics", L"Trilinear", c.trilinear) != 0;
    c.textureLodBias   = ReadDouble(L"Graphics", L"TextureLodBias", c.textureLodBias);
    c.dirtDetail       = ReadDouble(L"Graphics", L"DirtDetail", c.dirtDetail);
    c.dirtScale        = ReadDouble(L"Graphics", L"DirtScale", c.dirtScale);
    c.tireMarks        = ReadDouble(L"Graphics", L"TireMarks", c.tireMarks);
    c.markCacheSegs    = ReadDouble(L"Graphics", L"MarkCacheSegs", c.markCacheSegs);
    c.markDepth        = ReadDouble(L"Graphics", L"MarkDepth", c.markDepth);
    c.marksEveryBike   = ReadInt(L"Graphics", L"MarksEveryBike", c.marksEveryBike) != 0;
    c.headlightEveryBike = ReadInt(L"Graphics", L"HeadlightEveryBike", c.headlightEveryBike) != 0;
    c.headlightColor   = Clamp(ReadInt(L"Graphics", L"HeadlightColor", c.headlightColor), 0, 2);
    c.snow             = ReadDouble(L"Weather", L"Snow", c.snow ? 1.0 : 0.0) > 0.0;  // (any amount it was a slider for: on)
    c.snowfall         = Clamp(ReadDouble(L"Weather", L"Snowfall", c.snowfall), 0.0, 1.0);
    c.snowWind         = Clamp(ReadDouble(L"Weather", L"Wind", c.snowWind), 0.0, 1.0);
    c.tireDirt         = ReadDouble(L"Graphics", L"TireDirt", c.tireDirt);
    c.motionBlur       = ReadDouble(L"Graphics", L"MotionBlur", c.motionBlur);
    c.depthOfField     = ReadDouble(L"Graphics", L"DepthOfField", c.depthOfField);
    c.dofFocus         = ReadDouble(L"Graphics", L"DofFocus", c.dofFocus);
    c.hideHud          = ReadInt(L"Other", L"HideHud", c.hideHud) != 0;
    c.hideCursor       = ReadInt(L"Other", L"HideCursor", c.hideCursor) != 0;
    c.dabIndicator     = ReadInt(L"Other", L"DabIndicator", c.dabIndicator) != 0;
    c.brakeSqueal      = Clamp(ReadDouble(L"Other", L"BrakeSqueal", c.brakeSqueal), 0.0, 1.0);
    c.dabSticky        = Clamp(ReadInt(L"Other", L"DabSticky", c.dabSticky), 0, 2);
    c.fadeMenu         = ReadInt(L"Other", L"FadeMenu", c.fadeMenu) != 0;
    c.fullscreenWindow = ReadInt(L"Other", L"FullscreenWindow", c.fullscreenWindow) != 0;
    c.dumpShaders      = ReadInt(L"Graphics", L"DumpShaders", c.dumpShaders) != 0;

    // Keep every value inside a range the physics can't blow up with.
    c.viewMatrixIndex      = Clamp(c.viewMatrixIndex, 0, 10000);
    c.snapAngleDeg         = Clamp(c.snapAngleDeg, 1.0, 180.0);
    c.teleportDistance     = Clamp(c.teleportDistance, 0.01, 1e6);
    c.rotationResponse     = Clamp(c.rotationResponse, 0.0, 1000.0);
    c.maxLagDeg            = Clamp(c.maxLagDeg, 0.0, 90.0);
    c.bikeSearchRadius     = Clamp(c.bikeSearchRadius, 0.5, 100.0);
    c.cameraAngleDeg       = Clamp(c.cameraAngleDeg, -45.0, 60.0);
    c.headForward          = Clamp(c.headForward, -5.0, 5.0);
    c.headHeight           = Clamp(c.headHeight, -5.0, 5.0);
    c.horizonLock          = Clamp(c.horizonLock, 0.0, 1.0);
    c.pitchLock            = Clamp(c.pitchLock, 0.0, 1.0);
    c.maxRollCorrectionDeg = Clamp(c.maxRollCorrectionDeg, 0.0, 180.0);
    c.steadiness           = Clamp(c.steadiness, 0.0, 2.0);
    c.airLooseness         = Clamp(c.airLooseness, 0.0, 1.0);
    c.whipLock             = Clamp(c.whipLock, 0.0, 1.0);
    c.lookSideDeg          = Clamp(c.lookSideDeg, 10.0, 90.0);
    c.lookTurnSpeed        = Clamp(c.lookTurnSpeed, 2.0, 40.0);
    c.apexLook             = Clamp(c.apexLook, 0.0, 1.0);
    c.apexSpeed            = Clamp(c.apexSpeed, 0.5, 5.0);
    c.apexAnticipation     = 0.0;  // work in progress: off for now, whatever the ini says
    c.lookSideForward      = Clamp(c.lookSideForward, -5.0, 5.0);
    c.lookSideHeight       = Clamp(c.lookSideHeight, -5.0, 5.0);
    c.lookBackForward      = Clamp(c.lookBackForward, -5.0, 5.0);
    c.lookBackHeight       = Clamp(c.lookBackHeight, -5.0, 5.0);
    c.verticalResponse     = Clamp(c.verticalResponse, 0.0, 1000.0);
    c.maxVerticalLag       = Clamp(c.maxVerticalLag, 0.0, 10.0);
    c.compressionPerG      = Clamp(c.compressionPerG, 0.0, 1.0);
    c.springHz             = Clamp(c.springHz, 0.0, 50.0);
    c.dampingRatio         = Clamp(c.dampingRatio, 0.0, 10.0);
    c.maxCompression       = Clamp(c.maxCompression, 0.0, 10.0);
    c.maxExtension         = Clamp(c.maxExtension, 0.0, 10.0);
    c.nodDegPerMeter       = Clamp(c.nodDegPerMeter, -1000.0, 1000.0);
    c.accelFilterHz        = Clamp(c.accelFilterHz, 0.0, 200.0);
    c.replayDistance       = Clamp(c.replayDistance, 0.5, 30.0);
    c.replayHeight         = Clamp(c.replayHeight, -2.0, 15.0);
    c.replayPitchDeg       = Clamp(c.replayPitchDeg, -45.0, 80.0);
    c.replayCamera         = Clamp(c.replayCamera, 0, 2);
    c.helmetHeight         = Clamp(c.helmetHeight, -2.0, 5.0);
    c.helmetForward        = Clamp(c.helmetForward, -3.0, 3.0);
    c.helmetTiltDeg        = Clamp(c.helmetTiltDeg, -30.0, 80.0);
    c.helmetMount          = Clamp(c.helmetMount, 0, 3);
    c.sideHeight           = Clamp(c.sideHeight, -2.0, 5.0);
    c.sideForward          = Clamp(c.sideForward, -3.0, 3.0);
    c.sideTiltDeg          = Clamp(c.sideTiltDeg, -30.0, 80.0);
    c.sideOut              = Clamp(c.sideOut, 0.0, 3.0);
    c.chestHeight          = Clamp(c.chestHeight, -3.0, 3.0);
    c.chestForward         = Clamp(c.chestForward, -3.0, 3.0);
    c.chestTiltDeg         = Clamp(c.chestTiltDeg, -30.0, 80.0);
    c.helmetSmoothing      = Clamp(c.helmetSmoothing, 0.0, 1.0);
    c.helmetLevel          = Clamp(c.helmetLevel, 0.0, 1.0);
    if (c.timeOfDay >= 0.0)
        c.timeOfDay        = std::fmod(c.timeOfDay, 24.0);
    else
        c.timeOfDay        = -1.0;
    c.nightBrightness      = Clamp(c.nightBrightness, 0.0, 2.0);
    c.shadows              = Clamp(c.shadows, 0.0, 1.0);
    c.shadowLength         = Clamp(c.shadowLength, 1.0, 30.0);
    c.shadowDistance       = Clamp(c.shadowDistance, 5.0, 200.0);
    c.shadowResolution     = c.shadowResolution < 0 ? 0 : c.shadowResolution > 2 ? 2 : c.shadowResolution;
    c.shadowMapSize        = 2048 << c.shadowResolution;
    c.shadowSoftness       = Clamp(c.shadowSoftness, 0.0, 1.0);
    c.headlight            = Clamp(c.headlight, 0.0, 1.0);
    c.headlightBeamDeg     = Clamp(c.headlightBeamDeg, 5.0, 90.0);
    c.headlightForward     = Clamp(c.headlightForward, -3.0, 3.0);
    c.headlightHeight      = Clamp(c.headlightHeight, -3.0, 3.0);
    c.headlightAimDeg      = Clamp(c.headlightAimDeg, -20.0, 15.0);
    c.ambientOcclusion     = Clamp(c.ambientOcclusion, 0.0, 1.0);
    c.aoRadius             = Clamp(c.aoRadius, 0.1, 5.0);
    c.sunRays              = Clamp(c.sunRays, 0.0, 1.0);
    c.bloom                = Clamp(c.bloom, 0.0, 1.0);
    c.bloomThreshold       = Clamp(c.bloomThreshold, 0.0, 1.0);
    c.exposure             = Clamp(c.exposure, -3.0, 3.0);
    c.contrast             = Clamp(c.contrast, -0.5, 0.5);
    c.saturation           = Clamp(c.saturation, -1.0, 1.0);
    c.vignette             = Clamp(c.vignette, 0.0, 1.0);
    c.antiAliasing         = Clamp(c.antiAliasing, 0, 2);
    c.sharpen              = Clamp(c.sharpen, 0.0, 1.0);
    c.anisotropy           = Clamp(c.anisotropy, 1.0, 16.0);
    c.textureLodBias       = Clamp(c.textureLodBias, -2.0, 1.0);
    c.dirtDetail           = Clamp(c.dirtDetail, 0.0, 1.0);
    c.dirtScale            = Clamp(c.dirtScale, 0.25, 4.0);
    c.tireMarks            = Clamp(c.tireMarks, 0.0, 1.0);
    c.markCacheSegs        = Clamp(std::round(c.markCacheSegs), 10000.0, 1000000.0);
    c.markDepth            = Clamp(c.markDepth, 0.0, 1.0);
    c.tireDirt             = Clamp(c.tireDirt, 0.0, 1.0);
    c.motionBlur           = Clamp(c.motionBlur, 0.0, 1.0);
    c.depthOfField         = Clamp(c.depthOfField, 0.0, 1.0);
    c.dofFocus             = Clamp(c.dofFocus, 0.0, 500.0);
    return c;
}

}  // namespace

void ReloadConfig()
{
    g_activePreset = 0;
    for (std::string& name : g_presetNames)
        name.clear();
    if (GetFileAttributesW(g_path) == INVALID_FILE_ATTRIBUTES) {
        Log("config: %ls not found, using built-in defaults", g_path);
        g_config = Config{};
        for (Config& preset : g_presets)
            preset = g_config;
        return;
    }
    const Config c = ParseConfig(Config{});  // from the defaults: keys missing from the file keep sane values
    g_config = c;
    // The presets: the active one is what was just read; the others from their own sections (what
    // a section doesn't have, or a preset never saved, is a copy of the active one).
    wchar_t buffer[16];
    GetPrivateProfileStringW(L"Presets", L"Active", L"1", buffer, _countof(buffer), g_path);
    g_activePreset = Clamp(_wtoi(buffer) - 1, 0, kPresetCount - 1);
    for (int i = 0; i < kPresetCount; ++i) {
        wchar_t key[16], name[64];
        swprintf_s(key, L"Name%d", i + 1);
        GetPrivateProfileStringW(L"Presets", key, L"", name, _countof(name), g_path);
        char narrow[64];
        WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof narrow, nullptr, nullptr);
        narrow[sizeof narrow - 1] = '\0';
        g_presetNames[i] = CleanPresetName(narrow);
    }
    for (int i = 0; i < kPresetCount; ++i) {
        g_presets[i] = c;
        if (i == g_activePreset)
            continue;
        g_presetSection = PresetSection(i);
        g_presets[i] = ParseConfig(c);
        g_presetSection = nullptr;
    }
    Log("config: loaded %ls (enabled=%d, %s camera, modelview='%s', camera program has '%s', up=%.2f,%.2f,%.2f)",
        g_path, c.enabled ? 1 : 0, c.customCamera ? "custom" : "game", c.modelviewUniform, c.cameraProgramUniform,
        c.worldUp[0], c.worldUp[1], c.worldUp[2]);
    Log("config: shaders %s (time of day %s%.2f, shadows %.2f, AO %.2f, bloom %.2f, AA %d, %gx anisotropic, dirt %.2f, "
        "motion blur %.2f, DOF %.2f)", c.shaders ? "on" : "off", c.timeOfDay < 0 ? "game " : "", c.timeOfDay < 0 ? 0.0 : c.timeOfDay,
        c.shadows, c.ambientOcclusion, c.bloom, c.antiAliasing, c.anisotropy, c.dirtDetail, c.motionBlur, c.depthOfField);
}

// ---- Preset codes ----------------------------------------------------------------------------
// One for the first person tab ("MXS-REMASTERED-CAM-") and one for the graphics tab
// ("MXS-REMASTERED-GFX-"): the other tabs are preferences, not shared. Then, in base64url, a version
// byte, the tab, and per setting of that tab
// that isn't at its default a 3-byte name (FNV-1a of "Section.Key", so settings can be added or
// reordered later) and its value (a little-endian float), and a checksum byte (a code cut short or
// mistyped isn't loaded).
namespace {

constexpr char     kCodePrefix[] = "MXS-REMASTERED-";
constexpr uint8_t  kCodeVersion  = 1;
constexpr char     kBase64[]     = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
struct CodeTab { int tab; const char* tag; };
constexpr CodeTab  kCodeTabs[] = { { 0, "CAM" }, { 1, "GFX" } };

// The menu tab a setting is on (overlay.cpp's kTabs: 0 first person, 1 graphics, 2 replay, 4 other).
int TabOf(const TunableValue& v)
{
    if (wcscmp(v.section, L"Graphics") == 0)
        return 1;
    if (wcscmp(v.section, L"Weather") == 0)
        return 2;
    if (wcscmp(v.section, L"Replay") == 0)
        return 3;
    if (wcscmp(v.section, L"Other") == 0)
        return 5;
    return 0;  // Camera, Smoothing, Leveling, Air, Look, Neck
}

const char* TagOf(int tab)
{
    for (const CodeTab& t : kCodeTabs)
        if (t.tab == tab)
            return t.tag;
    return nullptr;
}

uint32_t NameHash(const TunableValue& v)
{
    uint32_t h = 2166136261u;
    const auto Mix = [&](const wchar_t* s) {
        for (; *s; ++s)
            h = (h ^ static_cast<uint8_t>(*s)) * 16777619u;
    };
    Mix(v.section);
    Mix(L".");
    Mix(v.key);
    return h & 0xFFFFFFu;
}

uint8_t Checksum(const std::vector<uint8_t>& bytes)
{
    uint8_t sum = 0x5A;
    for (uint8_t b : bytes)
        sum = static_cast<uint8_t>((sum << 1 | sum >> 7) ^ b);
    return sum;
}

}  // namespace

bool HasPresetCode(int tab)
{
    return TagOf(tab) != nullptr;
}

std::string PresetCode(int tab)
{
    const char* tag = TagOf(tab);
    if (!tag)
        return std::string();
    const std::vector<TunableValue> now = Tunables(g_config), defaults = Tunables(Config{});
    std::vector<uint8_t> bytes{ kCodeVersion, static_cast<uint8_t>(tab) };
    for (size_t i = 0; i < now.size() && i < defaults.size(); ++i) {
        const float value = static_cast<float>(now[i].value);
        if (TabOf(now[i]) != tab || value == static_cast<float>(defaults[i].value))
            continue;
        const uint32_t h = NameHash(now[i]);
        uint32_t bits;
        std::memcpy(&bits, &value, 4);
        for (int k = 0; k < 3; ++k)
            bytes.push_back(static_cast<uint8_t>(h >> (8 * k)));
        for (int k = 0; k < 4; ++k)
            bytes.push_back(static_cast<uint8_t>(bits >> (8 * k)));
    }
    bytes.push_back(Checksum(bytes));
    std::string code = kCodePrefix;
    code += tag;
    code += '-';
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint32_t n = bytes[i] << 16 | (i + 1 < bytes.size() ? bytes[i + 1] << 8 : 0) |
                           (i + 2 < bytes.size() ? bytes[i + 2] : 0);
        const size_t chars = bytes.size() - i >= 3 ? 4 : bytes.size() - i + 1;
        for (size_t k = 0; k < chars; ++k)
            code += kBase64[(n >> (18 - 6 * k)) & 63];
    }
    return code;
}

PresetCodeResult LoadPresetCode(const char* text)
{
    PresetCodeResult result;
    // The first "MXS-REMASTERED-XXX-" with a tag we know (a whole message pasted is fine).
    const char* start = nullptr;
    int tab = -1;
    for (const char* at = text ? strstr(text, kCodePrefix) : nullptr; at && !start; at = strstr(at + 1, kCodePrefix)) {
        const char* tagAt = at + sizeof kCodePrefix - 1;
        for (const CodeTab& t : kCodeTabs)
            if (_strnicmp(tagAt, t.tag, 3) == 0 && tagAt[3] == '-') {
                start = tagAt + 4;
                tab = t.tab;
                break;
            }
    }
    if (!start)
        return result;
    result.tab = tab;
    // base64url, up to the first character that isn't
    std::vector<uint8_t> bytes;
    uint32_t n = 0;
    int bits = 0;
    for (const char* c = start; *c; ++c) {
        const char* at = strchr(kBase64, *c);
        if (!at)
            break;
        n = n << 6 | static_cast<uint32_t>(at - kBase64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<uint8_t>(n >> bits));
            n &= (1u << bits) - 1;
        }
    }
    if (bytes.size() < 3 || (bytes.size() - 3) % 7 != 0) {
        result.status = PresetCodeResult::Damaged;
        return result;
    }
    const uint8_t sum = bytes.back();
    bytes.pop_back();
    if (Checksum(bytes) != sum || (bytes[0] == kCodeVersion && bytes[1] != tab)) {
        result.status = PresetCodeResult::Damaged;
        return result;
    }
    if (bytes[0] != kCodeVersion) {
        result.status = PresetCodeResult::Newer;
        return result;
    }
    // Every setting of its tab, as the ini would hold it: the code's value, or the default for one it
    // doesn't name. Read the way the ini is (every value kept in range); the other tabs' are kept.
    const std::vector<TunableValue> defaults = Tunables(Config{});
    std::vector<std::pair<std::wstring, std::wstring>> values;
    for (const TunableValue& v : defaults) {
        if (TabOf(v) != tab)
            continue;
        wchar_t full[128], number[32];
        swprintf_s(full, L"%s.%s", v.section, v.key);
        swprintf_s(number, L"%.6g", v.value);
        values.emplace_back(full, number);
    }
    for (size_t i = 2; i + 7 <= bytes.size(); i += 7) {
        const uint32_t h = bytes[i] | bytes[i + 1] << 8 | bytes[i + 2] << 16;
        uint32_t raw = 0;
        for (int k = 0; k < 4; ++k)
            raw |= static_cast<uint32_t>(bytes[i + 3 + k]) << (8 * k);
        float value;
        std::memcpy(&value, &raw, 4);
        const TunableValue* name = nullptr;
        for (const TunableValue& v : defaults)
            if (TabOf(v) == tab && NameHash(v) == h) {
                name = &v;
                break;
            }
        if (!name || !std::isfinite(value)) {
            ++result.unknown;  // (a setting from a newer version)
            continue;
        }
        wchar_t full[128], number[32];
        swprintf_s(full, L"%s.%s", name->section, name->key);
        swprintf_s(number, L"%.6g", static_cast<double>(value));
        for (auto& v : values)
            if (v.first == full)
                v.second = number;
        ++result.settings;
    }
    g_codeValues = &values;
    const Config loaded = ParseConfig(g_config);
    g_codeValues = nullptr;
    CopyTunables(loaded, g_config);
    result.status = PresetCodeResult::Loaded;
    Log("config: loaded a %s code into preset %d (%d setting(s) off their defaults%s)", TagOf(tab), g_activePreset + 1,
        result.settings, result.unknown ? ", some from a newer version left out" : "");
    return result;
}

}  // namespace mxcam
