// brake.cpp - see brake.h.
//
//   The game writes a brake binding as "front_brake_key=(<from>,<to>)<what>" for an axis (a trigger:
//   <from> is where it starts to count, <to> where it's all the way) or "(k)<what>" for a key or
//   button (see dab.cpp for <what>). Its value here is 0..1 the way the game reads it.
//
//   The squeal: brake discs ring at a few kHz when the pads stick and slip on them. Here two ringing
//   tones (a main one with its overtones, and a higher one that comes and goes), a fast flutter in
//   their loudness, a pitch that wanders as the pads grab and let go, and a little scrape of noise
//   under them. Each time it starts it rings at a pitch of its own. It follows how hard you brake,
//   fading in and out (never clicking on or off).
#include "brake.h"

#include <windows.h>
#ifdef _WIN32
#include <mmsystem.h>
#else
#include <dlfcn.h>
#include <thread>
#endif

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "gamepad.h"
#include "log.h"
#include "paths.h"

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
#endif

namespace mxcam {
namespace brake {
namespace {

// ---- The game's brake controls -------------------------------------------------------------------
struct Binding {
    enum Kind { None, Key, XButton, XAxis, JoyButton, JoyAxis } kind = None;
    int    index = 0;          // the key (virtual-key code), button or axis
    double from = 0, to = 1;   // an axis: where it starts to count and where it's all the way
};

struct NamedKey { const char* name; int vk; };
const NamedKey kKeys[] = {
    { "Esc", VK_ESCAPE }, { "Backspace", VK_BACK }, { "Tab", VK_TAB }, { "Enter", VK_RETURN },
    { "Left Ctrl", VK_LCONTROL }, { "Right Ctrl", VK_RCONTROL }, { "Left Shift", VK_LSHIFT },
    { "Right Shift", VK_RSHIFT }, { "Left Alt", VK_LMENU }, { "Right Alt", VK_RMENU }, { "Space", VK_SPACE },
    { "Caps Lock", VK_CAPITAL }, { "Insert", VK_INSERT }, { "Delete", VK_DELETE }, { "Home", VK_HOME },
    { "End", VK_END }, { "Page Up", VK_PRIOR }, { "Page Down", VK_NEXT }, { "Up", VK_UP }, { "Down", VK_DOWN },
    { "Left", VK_LEFT }, { "Right", VK_RIGHT }, { "Pause", VK_PAUSE }, { "Num Lock", VK_NUMLOCK },
    { "Keypad /", VK_DIVIDE }, { "Keypad *", VK_MULTIPLY }, { "Keypad -", VK_SUBTRACT }, { "Keypad +", VK_ADD },
    { "Keypad Enter", VK_RETURN }, { "Keypad .", VK_DECIMAL },
    { "Mouse Left", VK_LBUTTON }, { "Mouse Middle", VK_MBUTTON }, { "Mouse Right", VK_RBUTTON },
};

// "<what> button 3" / "<what> axis 1": the number after the last such word.
bool NumberAfter(const char* what, const char* word, int& number)
{
    const char* last = nullptr;
    for (const char* at = strstr(what, word); at; at = strstr(at + 1, word))
        last = at;
    if (!last || !isdigit(static_cast<unsigned char>(last[strlen(word)])))
        return false;
    number = atoi(last + strlen(word));
    return true;
}

Binding Parse(const char* value)
{
    Binding b;
    const char* what = nullptr;
    bool axis = false;
    if (strncmp(value, "(k)", 3) == 0) {
        what = value + 3;
    } else if (value[0] == '(' && sscanf_s(value, "(%lf,%lf)", &b.from, &b.to) == 2) {
        what = strchr(value, ')');
        what = what ? what + 1 : nullptr;
        axis = true;
    }
    if (!what || !*what)
        return b;
    const bool xinput = strncmp(what, "XInputGamepad", 13) == 0;
    int n = 0;
    if (NumberAfter(what, " button ", n)) {
        b.kind = xinput ? Binding::XButton : Binding::JoyButton;
        b.index = n;
        return b;
    }
    if (NumberAfter(what, " axis ", n)) {
        b.kind = xinput ? Binding::XAxis : Binding::JoyAxis;
        b.index = n;
        return b;
    }
    if (axis)
        return b;
    int vk = 0;
    for (const NamedKey& k : kKeys)
        if (_stricmp(what, k.name) == 0)
            vk = k.vk;
    if (!vk && strncmp(what, "Keypad ", 7) == 0 && isdigit(static_cast<unsigned char>(what[7])) && !what[8])
        vk = VK_NUMPAD0 + (what[7] - '0');
    if (!vk && what[0] == 'F' && isdigit(static_cast<unsigned char>(what[1]))) {
        const int f = atoi(what + 1);
        if (f >= 1 && f <= 24)
            vk = VK_F1 + f - 1;
    }
    if (!vk && what[0] && !what[1]) {
        const unsigned char c = static_cast<unsigned char>(what[0]);
        if (isalpha(c))
            vk = toupper(c);
        else if (isdigit(c))
            vk = c;
        else if (const SHORT scan = VkKeyScanA(static_cast<CHAR>(c)); scan != -1)
            vk = scan & 0xFF;
    }
    if (vk) {
        b.kind = Binding::Key;
        b.index = vk;
    }
    return b;
}

bool GameHasFocus()
{
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
}

// How far the control is pressed, 0..1, as the game reads it.
double ValueOf(const Binding& b)
{
    const auto Range = [&b](double raw) {
        const double span = b.to - b.from;
        return std::fabs(span) < 1e-6 ? 0.0 : std::fmin(std::fmax((raw - b.from) / span, 0.0), 1.0);
    };
    switch (b.kind) {
    case Binding::Key:
        return (GetAsyncKeyState(b.index) & 0x8000) ? 1.0 : 0.0;
    case Binding::XButton:
        return gamepad::IsDown((1 << 8) | b.index) ? 1.0 : 0.0;
    case Binding::JoyButton:
        return gamepad::IsDown((5 << 8) | b.index) ? 1.0 : 0.0;
    case Binding::XAxis:
        return Range(gamepad::XInputAxis(b.index));
    case Binding::JoyAxis:
        return Range(gamepad::JoyAxis(b.index));
    default:
        return 0.0;
    }
}

Binding  g_front, g_rear;
FILETIME g_written{};
double   g_checked = -1;

// The game's settings file, read again when it changes (checked once a second).
void ReadBindings(double now)
{
    if (g_checked >= 0 && now - g_checked < 1.0)
        return;
    g_checked = now;
    if (!paths::PersonalFolder()[0])
        return;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%sconfig", paths::PersonalFolder());
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info) || CompareFileTime(&info.ftLastWriteTime, &g_written) == 0)
        return;
    g_written = info.ftLastWriteTime;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f)
        return;
    char line[512], front[160] = "", rear[160] = "";
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "front_brake_key=", 16) == 0)
            strncpy_s(front, line + 16, _TRUNCATE);
        else if (strncmp(line, "rear_brake_key=", 15) == 0)
            strncpy_s(rear, line + 15, _TRUNCATE);
    }
    fclose(f);
    g_front = Parse(front);
    g_rear = Parse(rear);
    Log("brake squeal: the game's brakes are %s (front) and %s (rear)%s", front[0] ? front : "not set",
        rear[0] ? rear : "not set", g_front.kind == Binding::None && g_rear.kind == Binding::None ? ": NOT READ" : "");
}

// ---- The sound -------------------------------------------------------------------------------------
constexpr int kRate    = 44100;
constexpr int kBlock   = 512;   // samples a buffer (~12 ms)
constexpr int kBuffers = 4;     // queued (~46 ms ahead)

std::atomic<float> g_target{ 0.0f };  // how hard it squeals, 0..1 (from Update)
std::atomic<float> g_volume{ 0.0f };  // Config::brakeSqueal
std::atomic<bool>  g_stop{ false };
#ifdef _WIN32
HANDLE g_thread = nullptr;
#endif

struct Synth {
    double   level = 0;              // how loud, following g_target
    double   phase1 = 0, phase2 = 0, flutter = 0;
    double   pitch = 2450;           // this squeal's own ring (Hz)
    double   wander = 0, wanderTo = 0, upper = 0, upperTo = 0;
    int      untilNext = 0;          // samples to the next wander / upper target
    double   scrape = 0;             // the noise, high-passed
    double   lastNoise = 0;
    unsigned seed = 0x9E3779B9u;

    double Random()  // 0..1
    {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 8) / 16777216.0;
    }

    void Fill(short* out, int count, float target, float volume)
    {
        constexpr double kTwoPi = 6.283185307179586;
        const double attack = 1.0 - std::exp(-1.0 / (0.07 * kRate)), release = 1.0 - std::exp(-1.0 / (0.12 * kRate));
        for (int i = 0; i < count; ++i) {
            const double was = level;
            level += (target - level) * (target > level ? attack : release);
            if (was < 0.002 && level >= 0.002)
                pitch = 2100.0 + 700.0 * Random();  // a new squeal: its own ring
            if (level < 0.0005) {
                out[i] = 0;
                continue;
            }
            if (--untilNext <= 0) {  // the pads grabbing and letting go: the pitch wanders, the upper ring comes and goes
                untilNext = 300 + static_cast<int>(Random() * 900.0);
                wanderTo = (Random() - 0.5) * 0.05;
                upperTo = Random() < 0.35 ? 0.4 + 0.6 * Random() : 0.0;
            }
            wander += (wanderTo - wander) * 0.002;
            upper += (upperTo - upper) * 0.0015;
            const double f1 = pitch * (1.0 + wander), f2 = pitch * 1.43 * (1.0 + wander * 0.7);
            phase1 = std::fmod(phase1 + kTwoPi * f1 / kRate, kTwoPi);
            phase2 = std::fmod(phase2 + kTwoPi * f2 / kRate, kTwoPi);
            flutter = std::fmod(flutter + kTwoPi * (26.0 + 14.0 * level) / kRate, kTwoPi);
            const double ring = std::sin(phase1) + 0.32 * std::sin(2.0 * phase1) + 0.12 * std::sin(3.0 * phase1);
            const double tone = ring * (1.0 - 0.5 * upper) + 0.8 * upper * std::sin(phase2);
            const double am = 0.72 + 0.28 * std::sin(flutter);
            const double noise = Random() * 2.0 - 1.0;
            scrape = 0.6 * (scrape + noise - lastNoise);  // high-passed: a hiss, not a rumble
            lastNoise = noise;
            const double s = std::tanh((tone * am * 0.42 + scrape * 0.07) * level * 1.6) * volume;
            out[i] = static_cast<short>(s * 26000.0);
        }
    }
};

#ifdef _WIN32
DWORD WINAPI AudioThread(void*)
{
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = kRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = kRate * 2;
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HWAVEOUT device = nullptr;
    if (!ready || waveOutOpen(&device, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(ready), 0, CALLBACK_EVENT) !=
                      MMSYSERR_NOERROR) {
        Log("brake squeal: the sound output couldn't be opened");
        if (ready)
            CloseHandle(ready);
        return 0;
    }
    Log("brake squeal: sound output open (%d Hz)", kRate);
    static short samples[kBuffers][kBlock];
    WAVEHDR headers[kBuffers]{};
    Synth synth;
    for (int i = 0; i < kBuffers; ++i) {
        headers[i].lpData = reinterpret_cast<LPSTR>(samples[i]);
        headers[i].dwBufferLength = sizeof samples[i];
        waveOutPrepareHeader(device, &headers[i], sizeof headers[i]);
        synth.Fill(samples[i], kBlock, 0.0f, 0.0f);
        waveOutWrite(device, &headers[i], sizeof headers[i]);
    }
    while (!g_stop.load()) {
        WaitForSingleObject(ready, 50);
        for (int i = 0; i < kBuffers; ++i) {
            if (!(headers[i].dwFlags & WHDR_DONE))
                continue;
            synth.Fill(samples[i], kBlock, g_target.load(), g_volume.load());
            waveOutWrite(device, &headers[i], sizeof headers[i]);
        }
    }
    waveOutReset(device);
    for (int i = 0; i < kBuffers; ++i)
        waveOutUnprepareHeader(device, &headers[i], sizeof headers[i]);
    waveOutClose(device);
    CloseHandle(ready);
    Log("brake squeal: sound output closed");
    return 0;
}

void StartAudio()
{
    if (g_thread)
        return;
    g_stop = false;
    g_thread = CreateThread(nullptr, 0, AudioThread, nullptr, 0, nullptr);
}

void StopAudio()
{
    if (!g_thread)
        return;
    g_stop = true;
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = nullptr;
}

#else
// Linux: the same through ALSA (the game's own sound library: its functions found at run time),
// a block at a time, ALSA pacing the thread.
void AudioThread()
{
    using OpenFn = int (*)(void**, const char*, int, int);
    using SetParamsFn = int (*)(void*, int, int, unsigned, unsigned, int, unsigned);
    using WriteFn = long (*)(void*, const void*, unsigned long);
    using RecoverFn = int (*)(void*, int, int);
    using CloseFn = int (*)(void*);
    void* lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_NOLOAD);
    if (!lib)
        lib = dlopen("libasound.so.2", RTLD_NOW);
    const auto open = lib ? reinterpret_cast<OpenFn>(dlsym(lib, "snd_pcm_open")) : nullptr;
    const auto setParams = lib ? reinterpret_cast<SetParamsFn>(dlsym(lib, "snd_pcm_set_params")) : nullptr;
    const auto write = lib ? reinterpret_cast<WriteFn>(dlsym(lib, "snd_pcm_writei")) : nullptr;
    const auto recover = lib ? reinterpret_cast<RecoverFn>(dlsym(lib, "snd_pcm_recover")) : nullptr;
    const auto close = lib ? reinterpret_cast<CloseFn>(dlsym(lib, "snd_pcm_close")) : nullptr;
    void* pcm = nullptr;
    // (Playback 0; signed 16-bit little endian 2; interleaved 3; resampled if need be; ~46 ms queued.)
    if (!open || !setParams || !write || !close || open(&pcm, "default", 0, 0) < 0 ||
        setParams(pcm, 2, 3, 1, kRate, 1, kBuffers * kBlock * 1000000u / kRate) < 0) {
        Log("brake squeal: the sound output couldn't be opened (ALSA)");
        if (pcm && close)
            close(pcm);
        return;
    }
    Log("brake squeal: sound output open (%d Hz, ALSA)", kRate);
    static short samples[kBlock];
    Synth synth;
    while (!g_stop.load()) {
        synth.Fill(samples, kBlock, g_target.load(), g_volume.load());
        long n = write(pcm, samples, kBlock);
        if (n < 0 && recover)
            n = recover(pcm, static_cast<int>(n), 1);
        if (n < 0) {
            Log("brake squeal: the sound output stopped (ALSA error %ld)", n);
            break;
        }
    }
    close(pcm);
    Log("brake squeal: sound output closed");
}

std::thread g_audio;

void StartAudio()
{
    if (g_audio.joinable())
        return;
    g_stop = false;
    g_audio = std::thread(AudioThread);
}

void StopAudio()
{
    if (!g_audio.joinable())
        return;
    g_stop = true;
    g_audio.join();
}

#endif

// ---- When it squeals ------------------------------------------------------------------------------
double g_lastNow = -1, g_lastSpeed = 0, g_decel = 0;
bool   g_haveLast = false;

double SmoothStep(double a, double b, double x)
{
    const double t = std::fmin(std::fmax((x - a) / (b - a), 0.0), 1.0);
    return t * t * (3.0 - 2.0 * t);
}

}  // namespace

void Update(bool haveSpeed, double speed)
{
    static LARGE_INTEGER frequency{}, start{};
    if (!frequency.QuadPart) {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
    }
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    const double now = double(count.QuadPart - start.QuadPart) / double(frequency.QuadPart);
    const Config& cfg = GetConfig();
    if (cfg.brakeSqueal <= 0.0) {
        g_target = 0.0f;
        StopAudio();
        g_haveLast = false;
        return;
    }
    ReadBindings(now);
    StartAudio();
    // The slider follows loudness as it's heard (a high ring stays loud at a low level): its square,
    // and 100% half of full scale. 1% is next to nothing, 10% quiet, 50% a moderate squeal.
    g_volume = static_cast<float>(0.5 * cfg.brakeSqueal * cfg.brakeSqueal);
    // How hard you're slowing down (m/s^2), from your speed frame to frame, smoothed a little.
    const double dt = g_lastNow >= 0 ? now - g_lastNow : 0.0;
    g_lastNow = now;
    if (haveSpeed && g_haveLast && dt > 1e-4 && dt < 0.25) {
        const double a = 1.0 - std::exp(-dt / 0.12);
        g_decel += ((g_lastSpeed - speed) / dt - g_decel) * a;
    } else if (!haveSpeed) {
        g_decel = 0;
    }
    g_lastSpeed = speed;
    g_haveLast = haveSpeed;
    // Braking hard, moving, and really slowing down (not the brake held in the air, or standing),
    // with the game in front.
    const double brake = std::fmax(ValueOf(g_front), ValueOf(g_rear));
    double target = 0.0;
    if (haveSpeed && GameHasFocus())
        target = SmoothStep(0.35, 0.9, brake) * SmoothStep(1.5, 5.0, speed) * SmoothStep(1.0, 3.5, g_decel);
    g_target = static_cast<float>(target);
}

bool NeedsGamepad()
{
    const auto Pad = [](const Binding& b) { return b.kind == Binding::XButton || b.kind == Binding::XAxis ||
                                                   b.kind == Binding::JoyButton || b.kind == Binding::JoyAxis; };
    return GetConfig().brakeSqueal > 0.0 && (Pad(g_front) || Pad(g_rear) || g_checked < 0);
}

}  // namespace brake
}  // namespace mxcam
