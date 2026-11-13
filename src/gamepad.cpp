// gamepad.cpp
//   See gamepad.h. Every API is loaded on first use, so a system without one just reads no
//   controller through it. Asking XInput or the joystick API about an empty slot can take a
//   millisecond or more, so only the controllers in use are polled each frame; a background thread
//   searches the other slots once a second, so the search can't hitch a frame. Windows.Gaming.Input
//   (for the paddles) is read on a thread of its own, with the Windows Runtime started there and not
//   on the game's thread.
//
//   A Control is (kind << 8) | index:
//     kXButton  an Xbox-style pad's button, index = its bit in XINPUT_GAMEPAD::wButtons (the d-pad's
//               four bits also take another controller's hat)
//     kXTrigger 0 = left, 1 = right
//     kXAxis    axis * 2 + (1 for the negative way): left stick X, Y, right stick X, Y
//     kPaddle   an Xbox Elite controller's paddles, 0..3
//     kJoyButton  another controller's buttons, 0..31
//     kJoyAxis    axis * 2 + (1 for the negative way): its X, Y, Z, R, U, V axes
#include "gamepad.h"

#include <windows.h>
#ifdef _WIN32
#include <mmsystem.h>
#include <xinput.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.gaming.input.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <linux/joystick.h>
#include <sys/ioctl.h>
#include <unistd.h>
#define JOY_POVCENTERED 0xFFFF
#endif

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "log.h"

namespace mxcam {
namespace gamepad {
namespace {

enum Kind { kXButton = 1, kXTrigger, kXAxis, kPaddle, kJoyButton, kJoyAxis };
constexpr Control Make(int kind, int index) { return (kind << 8) | index; }
constexpr int KindOf(Control c) { return c >> 8; }
constexpr int IndexOf(Control c) { return c & 0xFF; }

constexpr double kPressed = 0.5;  // a stick or trigger counts as pressed past this

#ifdef _WIN32
typedef DWORD(WINAPI* XInputGetStateFn)(DWORD, XINPUT_STATE*);
typedef MMRESULT(WINAPI* JoyGetPosExFn)(UINT, LPJOYINFOEX);
typedef MMRESULT(WINAPI* JoyGetDevCapsFn)(UINT_PTR, LPJOYCAPSW, UINT);

constexpr DWORD kSearchIntervalMs = 1000;
constexpr UINT  kJoysticks        = 16;  // ids the joystick API hands out

bool             g_started = false;
XInputGetStateFn g_xinputGetState = nullptr;
JoyGetPosExFn    g_joyGetPosEx = nullptr;
JoyGetDevCapsFn  g_joyGetDevCaps = nullptr;
std::atomic<int> g_xinputPad{ -1 };  // XInput slot in use, -1 = none
std::atomic<int> g_joystick{ -1 };   // joystick id in use, -1 = none
std::atomic<unsigned> g_paddles{ 0 };     // bits 0..3, from the Windows.Gaming.Input thread
std::atomic<bool>     g_havePaddlePad{ false };

#else
constexpr double kSearchInterval = 1.0;  // s between looks for a controller plugged in
bool g_started = false;

double NowSeconds()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / 1e9;
}
#endif

#ifdef _WIN32
// The joystick in use: its axes' ranges (from its caps), which it has.
struct JoyAxes {
    DWORD min[6] = {}, max[6] = {};
    bool  has[6] = {};
};
JoyAxes g_joyAxes;
int     g_joyAxesFor = -1;

#endif

// The last Poll().
struct State {
    bool   x = false;      // an Xbox-style pad was read
    WORD   buttons = 0;
    double lt = 0, rt = 0;
    double axes[4] = {};   // -1..1: left X, left Y (+ = up), right X, right Y
    bool   joy = false;    // another controller (or the same one, through the joystick API)
    DWORD  joyButtons = 0;
    DWORD  pov = JOY_POVCENTERED;
    double joyAxis[6] = {};  // -1..1
    unsigned paddles = 0;
};
State g_state;

// Every control, in the order a capture takes them when several go at once (an Xbox pad shows
// up through the joystick API as well: its own names come first).
Control g_all[96];
int     g_allCount = 0;
bool    g_held[96] = {};   // capture: held when it started, and not let go since

void ListControls()
{
    if (g_allCount)
        return;
    const int xbuttons[] = { 2, 3, 1, 0, 12, 13, 14, 15, 8, 9, 6, 7, 5, 4 };  // d-pad L R D U, A B X Y, LB RB, LS RS, Back Start
    for (int b : xbuttons)
        g_all[g_allCount++] = Make(kXButton, b);
    for (int t = 0; t < 2; ++t)
        g_all[g_allCount++] = Make(kXTrigger, t);
    for (int a = 0; a < 8; ++a)
        g_all[g_allCount++] = Make(kXAxis, a);
    for (int p = 0; p < 4; ++p)
        g_all[g_allCount++] = Make(kPaddle, p);
    for (int b = 0; b < 32; ++b)
        g_all[g_allCount++] = Make(kJoyButton, b);
    for (int a = 0; a < 12; ++a)
        g_all[g_allCount++] = Make(kJoyAxis, a);
}

#ifdef _WIN32
bool ReadXInput(int pad, State& out)
{
    XINPUT_STATE state{};
    if (g_xinputGetState(DWORD(pad), &state) != ERROR_SUCCESS)
        return false;
    const XINPUT_GAMEPAD& g = state.Gamepad;
    out.x = true;
    out.buttons = g.wButtons;
    out.lt = g.bLeftTrigger / 255.0;
    out.rt = g.bRightTrigger / 255.0;
    const auto Axis = [](SHORT v) { return v < 0 ? v / 32768.0 : v / 32767.0; };
    out.axes[0] = Axis(g.sThumbLX);
    out.axes[1] = Axis(g.sThumbLY);
    out.axes[2] = Axis(g.sThumbRX);
    out.axes[3] = Axis(g.sThumbRY);
    return true;
}

void LoadJoyAxes(int id)
{
    g_joyAxes = JoyAxes{};
    g_joyAxesFor = id;
    JOYCAPSW caps{};
    if (!g_joyGetDevCaps || g_joyGetDevCaps(UINT_PTR(id), &caps, sizeof caps) != JOYERR_NOERROR)
        return;
    const UINT mins[6] = { caps.wXmin, caps.wYmin, caps.wZmin, caps.wRmin, caps.wUmin, caps.wVmin };
    const UINT maxs[6] = { caps.wXmax, caps.wYmax, caps.wZmax, caps.wRmax, caps.wUmax, caps.wVmax };
    const bool has[6] = { true, true, (caps.wCaps & JOYCAPS_HASZ) != 0, (caps.wCaps & JOYCAPS_HASR) != 0,
                          (caps.wCaps & JOYCAPS_HASU) != 0, (caps.wCaps & JOYCAPS_HASV) != 0 };
    for (int i = 0; i < 6; ++i) {
        g_joyAxes.min[i] = mins[i];
        g_joyAxes.max[i] = maxs[i];
        g_joyAxes.has[i] = has[i] && maxs[i] > mins[i];
    }
}

bool ReadJoystick(int id, State& out)
{
    JOYINFOEX info{};
    info.dwSize = sizeof info;
    info.dwFlags = JOY_RETURNALL;
    if (g_joyGetPosEx(UINT(id), &info) != JOYERR_NOERROR)
        return false;
    if (g_joyAxesFor != id)
        LoadJoyAxes(id);
    out.joy = true;
    out.joyButtons = info.dwButtons;
    out.pov = info.dwPOV;
    const DWORD pos[6] = { info.dwXpos, info.dwYpos, info.dwZpos, info.dwRpos, info.dwUpos, info.dwVpos };
    for (int i = 0; i < 6; ++i) {
        if (!g_joyAxes.has[i]) {
            out.joyAxis[i] = 0;
            continue;
        }
        const double mid = 0.5 * (double(g_joyAxes.min[i]) + double(g_joyAxes.max[i]));
        const double half = 0.5 * (double(g_joyAxes.max[i]) - double(g_joyAxes.min[i]));
        out.joyAxis[i] = (double(pos[i]) - mid) / half;
    }
    return true;
}

#endif

// The joystick API reports the d-pad as a hat: centred, or an angle in hundredths of a degree
// clockwise from up. Each direction takes the 90 deg around it; the diagonals into down count
// as down.
bool HatDir(DWORD pov, int bit)
{
    if (pov > 35999)
        return false;
    switch (bit) {
    case 0: return pov < 4500 || pov > 31500;      // up
    case 1: return pov >= 13500 && pov <= 22500;   // down
    case 2: return pov > 22500 && pov < 31500;     // left
    case 3: return pov > 4500 && pov < 13500;      // right
    default: return false;
    }
}

bool Down(const State& s, Control c)
{
    const int i = IndexOf(c);
    switch (KindOf(c)) {
    case kXButton:
        return (s.x && (s.buttons & (1u << i))) || (i < 4 && s.joy && HatDir(s.pov, i));
    case kXTrigger:
        return s.x && (i ? s.rt : s.lt) > kPressed;
    case kXAxis:
        return s.x && i < 8 && s.axes[i / 2] * ((i & 1) ? -1.0 : 1.0) > kPressed;
    case kPaddle:
        return (s.paddles & (1u << i)) != 0;
    case kJoyButton:
        return s.joy && i < 32 && (s.joyButtons & (1u << i));
    case kJoyAxis:
        return s.joy && i < 12 && s.joyAxis[i / 2] * ((i & 1) ? -1.0 : 1.0) > kPressed;
    default:
        return false;
    }
}

#ifdef _WIN32
// ---- Windows.Gaming.Input: only for the paddles ----
// Its own thread: it starts the Windows Runtime (multithreaded) there, never on the game's thread,
// and polls the first gamepad it lists at ~250 Hz (once a second while there is none).
typedef HRESULT(WINAPI* RoInitializeFn)(RO_INIT_TYPE);
typedef HRESULT(WINAPI* RoGetActivationFactoryFn)(HSTRING, REFIID, void**);
typedef HRESULT(WINAPI* WindowsCreateStringFn)(PCNZWCH, UINT32, HSTRING*);
typedef HRESULT(WINAPI* WindowsDeleteStringFn)(HSTRING);

DWORD WINAPI PaddleThread(void*)
{
    using namespace ABI::Windows::Gaming::Input;
    using ABI::Windows::Foundation::Collections::IVectorView;
    HMODULE combase = LoadLibraryW(L"combase.dll");
    if (!combase)
        return 0;
    const auto roInitialize = reinterpret_cast<RoInitializeFn>(GetProcAddress(combase, "RoInitialize"));
    const auto roGetFactory = reinterpret_cast<RoGetActivationFactoryFn>(GetProcAddress(combase, "RoGetActivationFactory"));
    const auto createString = reinterpret_cast<WindowsCreateStringFn>(GetProcAddress(combase, "WindowsCreateString"));
    const auto deleteString = reinterpret_cast<WindowsDeleteStringFn>(GetProcAddress(combase, "WindowsDeleteString"));
    if (!roInitialize || !roGetFactory || !createString || !deleteString || FAILED(roInitialize(RO_INIT_MULTITHREADED)))
        return 0;
    const wchar_t name[] = L"Windows.Gaming.Input.Gamepad";
    HSTRING className = nullptr;
    IGamepadStatics* statics = nullptr;
    if (SUCCEEDED(createString(name, UINT32(wcslen(name)), &className))) {
        roGetFactory(className, __uuidof(IGamepadStatics), reinterpret_cast<void**>(&statics));
        deleteString(className);
    }
    if (!statics) {
        Log("gamepad: Windows.Gaming.Input missing (no paddles)");
        return 0;
    }
    bool logged = false;
    for (;;) {
        IVectorView<Gamepad*>* pads = nullptr;
        IGamepad* pad = nullptr;
        unsigned size = 0;
        if (SUCCEEDED(statics->get_Gamepads(&pads)) && pads) {
            if (SUCCEEDED(pads->get_Size(&size)) && size > 0)
                pads->GetAt(0, &pad);
            pads->Release();
        }
        if (!pad) {
            g_paddles = 0;
            g_havePaddlePad = false;
            Sleep(kSearchIntervalMs);
            continue;
        }
        if (!logged) {
            Log("gamepad: Windows.Gaming.Input sees a controller (its paddles can be bound, if it has them)");
            logged = true;
        }
        g_havePaddlePad = true;
        // Read it for a second, then look at the list again (a pad unplugged or another plugged in).
        for (int i = 0; i < 250; ++i) {
            GamepadReading reading{};
            if (FAILED(pad->GetCurrentReading(&reading)))
                break;
            const unsigned b = static_cast<unsigned>(reading.Buttons);
            g_paddles = ((b & GamepadButtons_Paddle1) ? 1u : 0u) | ((b & GamepadButtons_Paddle2) ? 2u : 0u) |
                        ((b & GamepadButtons_Paddle3) ? 4u : 0u) | ((b & GamepadButtons_Paddle4) ? 8u : 0u);
            Sleep(4);
        }
        pad->Release();
    }
}

// XInput controllers are preferred for their own names; the joystick API is searched as well, for
// other controllers and for the buttons and axes an XInput pad doesn't report.
DWORD WINAPI SearchThread(void*)
{
    for (;;) {
        if (g_xinputPad < 0 && g_xinputGetState) {
            for (int i = 0; i < XUSER_MAX_COUNT; ++i) {
                State unused;
                if (ReadXInput(i, unused)) {
                    g_xinputPad = i;
                    Log("gamepad: using XInput controller %d", i);
                    break;
                }
            }
        }
        if (g_joystick < 0 && g_joyGetPosEx) {
            for (UINT id = 0; id < kJoysticks; ++id) {
                JOYINFOEX info{};
                info.dwSize = sizeof info;
                info.dwFlags = JOY_RETURNPOV;
                if (g_joyGetPosEx(id, &info) == JOYERR_NOERROR) {
                    g_joystick = int(id);
                    Log("gamepad: using joystick %u", id);
                    break;
                }
            }
        }
        Sleep(kSearchIntervalMs);
    }
}

void Start()
{
    g_started = true;
    ListControls();
    const wchar_t* const xinputs[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    for (const wchar_t* name : xinputs) {
        if (HMODULE dll = LoadLibraryW(name)) {
            g_xinputGetState = reinterpret_cast<XInputGetStateFn>(GetProcAddress(dll, "XInputGetState"));
            if (g_xinputGetState)
                break;
        }
    }
    if (HMODULE winmm = LoadLibraryW(L"winmm.dll")) {
        g_joyGetPosEx = reinterpret_cast<JoyGetPosExFn>(GetProcAddress(winmm, "joyGetPosEx"));
        g_joyGetDevCaps = reinterpret_cast<JoyGetDevCapsFn>(GetProcAddress(winmm, "joyGetDevCapsW"));
    }
    Log("gamepad: XInput %s, joystick API %s", g_xinputGetState ? "found" : "missing",
        g_joyGetPosEx ? "found" : "missing");
    if (g_xinputGetState || g_joyGetPosEx) {
        if (HANDLE thread = CreateThread(nullptr, 0, SearchThread, nullptr, 0, nullptr))
            CloseHandle(thread);
        else
            Log("gamepad: could not start the controller search (error %lu)", GetLastError());
    }
    if (HANDLE thread = CreateThread(nullptr, 0, PaddleThread, nullptr, 0, nullptr))
        CloseHandle(thread);
}

#else
// ---- Linux: the joystick devices (/dev/input/js*) ----
// The kernel says which of its codes each button and axis of a pad is (JSIOCGBTNMAP / AXMAP), so
// any gamepad - an Xbox one, a PlayStation one, most others - reads as the Xbox-style pad the
// controls are named after: A B X Y, the bumpers, the stick clicks, the sticks, the triggers and
// the d-pad (a hat or buttons). Its raw buttons and axes are kept as well (JOY_BUTTON / JOY_ axes).
struct LinuxPad {
    int      fd = -1;
    char     name[128] = "";
    uint8_t  axisCodes[ABS_CNT] = {};
    uint16_t buttonCodes[KEY_MAX - BTN_MISC + 1] = {};
    int      axes = 0, buttons = 0;
    int16_t  axis[ABS_CNT] = {};
    uint32_t pressed[(KEY_MAX - BTN_MISC + 1 + 31) / 32] = {};
    bool     gamepad = false;  // reads as an Xbox-style pad
};
LinuxPad g_pad;
double   g_nextSearch = 0;

void ClosePad()
{
    if (g_pad.fd >= 0)
        close(g_pad.fd);
    g_pad = LinuxPad{};
}

bool OpenPad(int index)
{
    char path[32];
    snprintf(path, sizeof path, "/dev/input/js%d", index);
    const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return false;
    LinuxPad pad;
    pad.fd = fd;
    uint8_t axes = 0, buttons = 0;
    ioctl(fd, JSIOCGAXES, &axes);
    ioctl(fd, JSIOCGBUTTONS, &buttons);
    pad.axes = axes;
    pad.buttons = buttons;
    if (ioctl(fd, JSIOCGNAME(sizeof pad.name), pad.name) < 0)
        snprintf(pad.name, sizeof pad.name, "controller %d", index);
    ioctl(fd, JSIOCGAXMAP, pad.axisCodes);
    ioctl(fd, JSIOCGBTNMAP, pad.buttonCodes);
    for (int b = 0; b < pad.buttons; ++b)
        if (pad.buttonCodes[b] == BTN_SOUTH)  // (a gamepad's A / cross)
            pad.gamepad = true;
    g_pad = pad;
    Log("gamepad: using %s (%s, %d axes, %d buttons)%s", path, pad.name, pad.axes, pad.buttons,
        pad.gamepad ? "" : ": not a gamepad layout, its buttons and axes are bound by number");
    return true;
}

void SearchPads(double now)
{
    if (g_pad.fd >= 0 || now < g_nextSearch)
        return;
    g_nextSearch = now + kSearchInterval;
    for (int i = 0; i < 8; ++i)
        if (OpenPad(i))
            return;
}

// The pad's events since the last frame (the first ones a pad sends are its state as it is).
bool ReadPad()
{
    js_event e;
    for (;;) {
        const ssize_t n = read(g_pad.fd, &e, sizeof e);
        if (n != static_cast<ssize_t>(sizeof e)) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR))
                return true;
            return false;  // (unplugged)
        }
        const int type = e.type & ~JS_EVENT_INIT;
        if (type == JS_EVENT_AXIS && e.number < ABS_CNT) {
            g_pad.axis[e.number] = e.value;
        } else if (type == JS_EVENT_BUTTON) {  // (its number fits: pressed[] holds 256)
            if (e.value)
                g_pad.pressed[e.number / 32] |= 1u << (e.number % 32);
            else
                g_pad.pressed[e.number / 32] &= ~(1u << (e.number % 32));
        }
    }
}

void FillState(State& out)
{
    const auto Pressed = [](int b) { return (g_pad.pressed[b / 32] & (1u << (b % 32))) != 0; };
    const auto Stick = [](int16_t v) { return v < 0 ? v / 32768.0 : v / 32767.0; };
    out.joy = true;
    for (int b = 0; b < g_pad.buttons && b < 32; ++b)
        if (Pressed(b))
            out.joyButtons |= 1u << b;
    for (int a = 0; a < 6 && a < g_pad.axes; ++a)
        out.joyAxis[a] = Stick(g_pad.axis[a]);
    int hatX = 0, hatY = 0;
    for (int a = 0; a < g_pad.axes; ++a) {
        if (g_pad.axisCodes[a] == ABS_HAT0X)
            hatX = g_pad.axis[a] < -16384 ? -1 : g_pad.axis[a] > 16384 ? 1 : 0;
        else if (g_pad.axisCodes[a] == ABS_HAT0Y)
            hatY = g_pad.axis[a] < -16384 ? -1 : g_pad.axis[a] > 16384 ? 1 : 0;
    }
    if (hatX || hatY) {  // (as the joystick API's hat: hundredths of a degree clockwise from up)
        static const DWORD kPov[3][3] = { { 31500, 27000, 22500 }, { 0, JOY_POVCENTERED, 18000 }, { 4500, 9000, 13500 } };
        out.pov = kPov[hatX + 1][hatY + 1];
    }
    if (!g_pad.gamepad)
        return;
    // As XInput's: its button bits, sticks up positive, triggers 0..1.
    out.x = true;
    WORD buttons = 0;
    for (int b = 0; b < g_pad.buttons; ++b) {
        if (!Pressed(b))
            continue;
        switch (g_pad.buttonCodes[b]) {
        case BTN_DPAD_UP:    buttons |= 0x0001; break;
        case BTN_DPAD_DOWN:  buttons |= 0x0002; break;
        case BTN_DPAD_LEFT:  buttons |= 0x0004; break;
        case BTN_DPAD_RIGHT: buttons |= 0x0008; break;
        case BTN_START:      buttons |= 0x0010; break;
        case BTN_SELECT:     buttons |= 0x0020; break;
        case BTN_THUMBL:     buttons |= 0x0040; break;
        case BTN_THUMBR:     buttons |= 0x0080; break;
        case BTN_TL:         buttons |= 0x0100; break;
        case BTN_TR:         buttons |= 0x0200; break;
        case BTN_SOUTH:      buttons |= 0x1000; break;  // A
        case BTN_EAST:       buttons |= 0x2000; break;  // B
        case BTN_NORTH:      buttons |= 0x4000; break;  // X (the kernel's BTN_X)
        case BTN_WEST:       buttons |= 0x8000; break;  // Y (the kernel's BTN_Y)
        default: break;
        }
    }
    if (hatY < 0) buttons |= 0x0001;
    if (hatY > 0) buttons |= 0x0002;
    if (hatX < 0) buttons |= 0x0004;
    if (hatX > 0) buttons |= 0x0008;
    out.buttons = buttons;
    for (int a = 0; a < g_pad.axes; ++a) {
        const double v = Stick(g_pad.axis[a]);
        switch (g_pad.axisCodes[a]) {
        case ABS_X:  out.axes[0] = v; break;
        case ABS_Y:  out.axes[1] = -v; break;
        case ABS_RX: out.axes[2] = v; break;
        case ABS_RY: out.axes[3] = -v; break;
        case ABS_Z:
        case ABS_BRAKE: out.lt = (v + 1.0) * 0.5; break;
        case ABS_RZ:
        case ABS_GAS:   out.rt = (v + 1.0) * 0.5; break;
        default: break;
        }
    }
}

void Start()
{
    g_started = true;
    ListControls();
}

#endif

struct Named { Control control; const char* key; const char* name; };
const Named* NameTable(int& count)
{
    static Named table[96];
    static char keys[96][24], names[96][28];
    static int n = 0;
    if (!n) {
        const struct { int bit; const char* key; const char* name; } xb[] = {
            { 2, "DPAD_LEFT", "D-PAD LEFT" }, { 3, "DPAD_RIGHT", "D-PAD RIGHT" }, { 1, "DPAD_DOWN", "D-PAD DOWN" },
            { 0, "DPAD_UP", "D-PAD UP" }, { 12, "A", "A" }, { 13, "B", "B" }, { 14, "X", "X" }, { 15, "Y", "Y" },
            { 8, "LB", "LEFT BUMPER" }, { 9, "RB", "RIGHT BUMPER" }, { 6, "LS_CLICK", "LEFT STICK CLICK" },
            { 7, "RS_CLICK", "RIGHT STICK CLICK" }, { 5, "BACK", "VIEW / BACK" }, { 4, "START", "MENU / START" },
        };
        for (const auto& b : xb)
            table[n++] = { Make(kXButton, b.bit), b.key, b.name };
        table[n++] = { Make(kXTrigger, 0), "LT", "LEFT TRIGGER" };
        table[n++] = { Make(kXTrigger, 1), "RT", "RIGHT TRIGGER" };
        const char* const axisKeys[8] = { "LS_X+", "LS_X-", "LS_Y+", "LS_Y-", "RS_X+", "RS_X-", "RS_Y+", "RS_Y-" };
        const char* const axisNames[8] = { "LEFT STICK RIGHT", "LEFT STICK LEFT", "LEFT STICK UP", "LEFT STICK DOWN",
                                           "RIGHT STICK RIGHT", "RIGHT STICK LEFT", "RIGHT STICK UP", "RIGHT STICK DOWN" };
        for (int a = 0; a < 8; ++a)
            table[n++] = { Make(kXAxis, a), axisKeys[a], axisNames[a] };
        for (int p = 0; p < 4; ++p) {
            snprintf(keys[n], sizeof keys[n], "PADDLE%d", p + 1);
            snprintf(names[n], sizeof names[n], "PADDLE %d", p + 1);
            table[n] = { Make(kPaddle, p), keys[n], names[n] };
            ++n;
        }
        for (int b = 0; b < 32; ++b) {
            snprintf(keys[n], sizeof keys[n], "JOY_BUTTON%d", b + 1);
            snprintf(names[n], sizeof names[n], "BUTTON %d", b + 1);
            table[n] = { Make(kJoyButton, b), keys[n], names[n] };
            ++n;
        }
        const char* const joyAxes = "XYZRUV";
        for (int a = 0; a < 12; ++a) {
            snprintf(keys[n], sizeof keys[n], "JOY_%c%c", joyAxes[a / 2], (a & 1) ? '-' : '+');
            snprintf(names[n], sizeof names[n], "AXIS %c%c", joyAxes[a / 2], (a & 1) ? '-' : '+');
            table[n] = { Make(kJoyAxis, a), keys[n], names[n] };
            ++n;
        }
    }
    count = n;
    return table;
}

}  // namespace

static_assert(kDPadLeft == Make(kXButton, 2) && kDPadRight == Make(kXButton, 3) && kDPadDown == Make(kXButton, 1),
              "gamepad.h's d-pad controls");

void Poll()
{
    if (!g_started)
        Start();
    State s;
#ifndef _WIN32
    SearchPads(NowSeconds());
    if (g_pad.fd >= 0 && !ReadPad()) {
        Log("gamepad: %s disconnected", g_pad.name);
        ClosePad();
    }
    if (g_pad.fd >= 0)
        FillState(s);
    g_state = s;
#else
    const int xinputPad = g_xinputPad;
    if (xinputPad >= 0 && !ReadXInput(xinputPad, s)) {
        Log("gamepad: XInput controller %d disconnected", xinputPad);
        g_xinputPad = -1;
    }
    const int joystick = g_joystick;
    if (joystick >= 0 && !ReadJoystick(joystick, s)) {
        Log("gamepad: joystick %d disconnected", joystick);
        g_joystick = -1;
        g_joyAxesFor = -1;
    }
    s.paddles = g_paddles;
    g_state = s;
#endif
}

bool IsDown(Control control) { return control != kNone && Down(g_state, control); }

double XInputAxis(int axis)
{
    if (!g_state.x)
        return 0.0;
    if (axis == 4)
        return g_state.lt;
    if (axis == 5)
        return g_state.rt;
    return axis >= 0 && axis < 4 ? g_state.axes[axis] : 0.0;
}

double JoyAxis(int axis) { return g_state.joy && axis >= 0 && axis < 6 ? g_state.joyAxis[axis] : 0.0; }

void StartCapture()
{
    ListControls();
    for (int i = 0; i < g_allCount; ++i)
        g_held[i] = Down(g_state, g_all[i]);
}

Control PollCapture()
{
    Control found = kNone;
    for (int i = 0; i < g_allCount; ++i) {
        const bool down = Down(g_state, g_all[i]);
        if (!down) {
            g_held[i] = false;
        } else if (!g_held[i]) {
            g_held[i] = true;  // (reported once per press)
            if (found == kNone)
                found = g_all[i];
        }
    }
    return found;
}

const char* ControlName(Control control)
{
    int count;
    const Named* table = NameTable(count);
    for (int i = 0; i < count; ++i)
        if (table[i].control == control)
            return table[i].name;
    return "NONE";
}

const char* ControlKey(Control control)
{
    int count;
    const Named* table = NameTable(count);
    for (int i = 0; i < count; ++i)
        if (table[i].control == control)
            return table[i].key;
    return "NONE";
}

Control ParseControl(const char* key)
{
    int count;
    const Named* table = NameTable(count);
    for (int i = 0; i < count; ++i)
        if (_stricmp(table[i].key, key) == 0)
            return table[i].control;
    return kNone;
}

const char* StatusText()
{
#ifdef _WIN32
    if (g_xinputPad >= 0)
        return "XBOX PAD";
    if (g_joystick >= 0)
        return "PAD";
#else
    if (g_pad.fd >= 0)
        return g_pad.gamepad ? "GAMEPAD" : "PAD";
#endif
    return "NO PAD";
}

}  // namespace gamepad
}  // namespace mxcam
