// input_linux.cpp (Linux build only)
//   The game's X11 window and the keyboard and mouse, for the menu and the hotkeys: what the
//   Windows build gets from GetAsyncKeyState, GetCursorPos and its window procedure.
//
//   - Key and button state: asked of the X server on a connection of our own (XQueryKeymap,
//     XQueryPointer), never on the game's, which its own thread may be using. A frame asks a few
//     hundred keys: the answer is kept for 2 ms.
//   - The game's input events (XNextEvent and the like, which the game calls and this library
//     stands in for): the wheel and Esc are noted, and while the menu is open the presses and the
//     pointer's moves don't reach the game (they're turned into an event it ignores). Releases
//     always reach it, so nothing it saw go down sticks.
//   - The clipboard: the usual tools (wl-copy / wl-paste on Wayland, xclip or xsel on X11).
//
//   libX11 is the game's own: its functions are found at run time (nothing to build against).
#include "platform_linux.h"
#include "win32_compat.h"

#include <atomic>
#include <dlfcn.h>
#include <mutex>
#include <unistd.h>

#include "../log.h"
#include "../overlay.h"

namespace {

// ---- The few Xlib types and constants used (Xlib's ABI) ----
typedef unsigned long XID;
typedef XID Window;
typedef unsigned long KeySym;
typedef unsigned char KeyCode;
typedef int Bool;
struct XDisplayTag;
typedef XDisplayTag Display;

struct XAnyEvent {
    int           type;
    unsigned long serial;
    Bool          send_event;
    Display*      display;
    Window        window;
};
struct XKeyEvent {
    int           type;
    unsigned long serial;
    Bool          send_event;
    Display*      display;
    Window        window, root, subwindow;
    unsigned long time;
    int           x, y, x_root, y_root;
    unsigned int  state;
    unsigned int  keycode;
    Bool          same_screen;
};
struct XButtonEvent {
    int           type;
    unsigned long serial;
    Bool          send_event;
    Display*      display;
    Window        window, root, subwindow;
    unsigned long time;
    int           x, y, x_root, y_root;
    unsigned int  state;
    unsigned int  button;
    Bool          same_screen;
};
struct XPropertyEvent {
    int           type;
    unsigned long serial;
    Bool          send_event;
    Display*      display;
    Window        window;
    unsigned long atom;
    unsigned long time;
    int           state;
};
union XEvent {
    int            type;
    XAnyEvent      xany;
    XKeyEvent      xkey;
    XButtonEvent   xbutton;
    XPropertyEvent xproperty;
    long           pad[24];
};
constexpr int kKeyPress = 2, kKeyRelease = 3, kButtonPress = 4, kMotionNotify = 6,
              kPropertyNotify = 28;
constexpr unsigned kButton1Mask = 1u << 8, kButton2Mask = 1u << 9, kButton3Mask = 1u << 10;

using PredicateFn = Bool (*)(Display*, XEvent*, char*);

template <class T>
T Next(const char* name)
{
    return reinterpret_cast<T>(dlsym(RTLD_NEXT, name));
}
template <class T>
T Any(const char* name)
{
    return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}

struct X {
    Display* (*OpenDisplay)(const char*) = nullptr;
    int (*QueryKeymap)(Display*, char[32]) = nullptr;
    Bool (*QueryPointer)(Display*, Window, Window*, Window*, int*, int*, int*, int*, unsigned*) = nullptr;
    int (*GetGeometry)(Display*, Window, Window*, int*, int*, unsigned*, unsigned*, unsigned*, unsigned*) = nullptr;
    int (*GetInputFocus)(Display*, Window*, int*) = nullptr;
    KeyCode (*KeysymToKeycode)(Display*, KeySym) = nullptr;
    KeySym (*KeycodeToKeysym)(Display*, KeyCode, unsigned, unsigned) = nullptr;  // XkbKeycodeToKeysym
    KeySym (*LookupKeysym)(XKeyEvent*, int) = nullptr;
    int (*QueryTree)(Display*, Window, Window*, Window*, Window**, unsigned*) = nullptr;
    int (*Free)(void*) = nullptr;
    bool loaded = false;
    bool ok = false;
};
X g_x;

bool LoadX()
{
    if (g_x.loaded)
        return g_x.ok;
    g_x.loaded = true;
    g_x.OpenDisplay = Any<decltype(g_x.OpenDisplay)>("XOpenDisplay");
    g_x.QueryKeymap = Any<decltype(g_x.QueryKeymap)>("XQueryKeymap");
    g_x.QueryPointer = Any<decltype(g_x.QueryPointer)>("XQueryPointer");
    g_x.GetGeometry = Any<decltype(g_x.GetGeometry)>("XGetGeometry");
    g_x.GetInputFocus = Any<decltype(g_x.GetInputFocus)>("XGetInputFocus");
    g_x.KeysymToKeycode = Any<decltype(g_x.KeysymToKeycode)>("XKeysymToKeycode");
    g_x.KeycodeToKeysym = Any<decltype(g_x.KeycodeToKeysym)>("XkbKeycodeToKeysym");
    g_x.LookupKeysym = Any<decltype(g_x.LookupKeysym)>("XLookupKeysym");
    g_x.QueryTree = Any<decltype(g_x.QueryTree)>("XQueryTree");
    g_x.Free = Any<decltype(g_x.Free)>("XFree");
    g_x.ok = g_x.OpenDisplay && g_x.QueryKeymap && g_x.QueryPointer && g_x.GetGeometry && g_x.KeysymToKeycode;
    if (!g_x.ok)
        mxcam::Log("input: libX11 not found in the game: the menu can't read the keyboard and mouse");
    return g_x.ok;
}

std::atomic<Display*>      g_gameDisplay{ nullptr };
std::atomic<unsigned long> g_gameWindow{ 0 };
Display*                   g_query = nullptr;  // our own connection (the menu's thread)
std::mutex                 g_queryLock;

Display* Query()
{
    if (g_query || !LoadX())
        return g_query;
    static bool tried = false;
    if (tried)
        return nullptr;
    tried = true;
    g_query = g_x.OpenDisplay(nullptr);  // ($DISPLAY: the game's)
    if (!g_query)
        mxcam::Log("input: could not open the X display: the menu can't read the keyboard and mouse");
    return g_query;
}

double Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / 1e9;
}

// ---- Windows virtual-key codes as X11 keysyms ----
struct VkSym { int vk; KeySym sym; KeySym alt; };
const VkSym kKeys[] = {
    { VK_BACK, 0xFF08, 0 },   { VK_TAB, 0xFF09, 0 },     { VK_RETURN, 0xFF0D, 0xFF8D },  // (Return, keypad Enter)
    { VK_PAUSE, 0xFF13, 0 },  { VK_CAPITAL, 0xFFE5, 0 }, { VK_ESCAPE, 0xFF1B, 0 },
    { VK_SPACE, 0x20, 0 },    { VK_PRIOR, 0xFF55, 0 },   { VK_NEXT, 0xFF56, 0 },
    { VK_END, 0xFF57, 0 },    { VK_HOME, 0xFF50, 0 },    { VK_LEFT, 0xFF51, 0 },
    { VK_UP, 0xFF52, 0 },     { VK_RIGHT, 0xFF53, 0 },   { VK_DOWN, 0xFF54, 0 },
    { VK_INSERT, 0xFF63, 0 }, { VK_DELETE, 0xFFFF, 0 },
    { VK_LWIN, 0xFFEB, 0 },   { VK_RWIN, 0xFFEC, 0 },
    { VK_MULTIPLY, 0xFFAA, 0 }, { VK_ADD, 0xFFAB, 0 },   { VK_SUBTRACT, 0xFFAD, 0 },
    { VK_DECIMAL, 0xFFAE, 0 },  { VK_DIVIDE, 0xFFAF, 0 }, { VK_NUMLOCK, 0xFF7F, 0 },
    { VK_SCROLL, 0xFF14, 0 },
    { VK_LSHIFT, 0xFFE1, 0 }, { VK_RSHIFT, 0xFFE2, 0 },  { VK_LCONTROL, 0xFFE3, 0 },
    { VK_RCONTROL, 0xFFE4, 0 }, { VK_LMENU, 0xFFE9, 0 }, { VK_RMENU, 0xFFEA, 0xFE03 },  // (Alt Gr)
    { VK_OEM_1, 0x3B, 0 },    { VK_OEM_PLUS, 0x3D, 0 },  { VK_OEM_COMMA, 0x2C, 0 },
    { VK_OEM_MINUS, 0x2D, 0 }, { VK_OEM_PERIOD, 0x2E, 0 }, { VK_OEM_2, 0x2F, 0 },
    { VK_OEM_3, 0x60, 0 },    { VK_OEM_4, 0x5B, 0 },     { VK_OEM_5, 0x5C, 0 },
    { VK_OEM_6, 0x5D, 0 },    { VK_OEM_7, 0x27, 0 },
};

bool SymsFor(int vk, KeySym& sym, KeySym& alt)
{
    sym = alt = 0;
    if (vk >= 'A' && vk <= 'Z') {
        sym = static_cast<KeySym>('a' + (vk - 'A'));
        return true;
    }
    if (vk >= '0' && vk <= '9') {
        sym = static_cast<KeySym>(vk);
        return true;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        sym = 0xFFBE + static_cast<KeySym>(vk - VK_F1);
        return true;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD0 + 9) {
        sym = 0xFFB0 + static_cast<KeySym>(vk - VK_NUMPAD0);
        return true;
    }
    for (const VkSym& k : kKeys) {
        if (k.vk == vk) {
            sym = k.sym;
            alt = k.alt;
            return true;
        }
    }
    return false;
}

int VkOfSym(KeySym sym)
{
    if (sym >= 'a' && sym <= 'z')
        return static_cast<int>('A' + (sym - 'a'));
    if (sym >= 'A' && sym <= 'Z')
        return static_cast<int>(sym);
    if (sym >= '0' && sym <= '9')
        return static_cast<int>(sym);
    for (const VkSym& k : kKeys)
        if (k.sym == sym || (k.alt && k.alt == sym))
            return k.vk;
    return 0;
}

// ---- The keyboard and pointer state, asked now and then ----
struct State {
    double        keysAt = -1, pointerAt = -1, geometryAt = -1, focusAt = -1;
    char          keys[32] = {};
    KeyCode       codes[256][2] = {};
    bool          codesKnown[256] = {};
    unsigned      buttons = 0;
    int           px = 0, py = 0;
    bool          pointerOk = false;
    unsigned      width = 0, height = 0;
    bool          geometryOk = false;
    bool          focused = true;
};
State g_state;

bool KeycodeDown(KeyCode code) { return code && (g_state.keys[code >> 3] & (1 << (code & 7))); }

void RefreshKeys(Display* d)
{
    const double now = Now();
    if (now - g_state.keysAt < 0.002)
        return;
    g_state.keysAt = now;
    g_x.QueryKeymap(d, g_state.keys);
}

void RefreshPointer(Display* d)
{
    const double now = Now();
    if (now - g_state.pointerAt < 0.002)
        return;
    g_state.pointerAt = now;
    const Window w = g_gameWindow.load();
    Window root = 0, child = 0;
    int rx = 0, ry = 0, wx = 0, wy = 0;
    unsigned mask = 0;
    g_state.pointerOk = w && g_x.QueryPointer(d, w, &root, &child, &rx, &ry, &wx, &wy, &mask);
    if (!w)  // (no window yet: the buttons, from the root)
        g_x.QueryPointer(d, 0, &root, &child, &rx, &ry, &wx, &wy, &mask);
    g_state.buttons = mask;
    g_state.px = wx;
    g_state.py = wy;
}

}  // namespace

namespace mxcam {
namespace platform {

void NoteGameWindow(void* display, unsigned long window)
{
    if (display)
        g_gameDisplay.store(static_cast<Display*>(display));
    if (window && g_gameWindow.exchange(window) != window) {
        static int logged = 0;
        if (logged++ < 4)
            Log("input: the game draws into X11 window 0x%lx", window);
        std::lock_guard<std::mutex> lock(g_queryLock);
        g_state.geometryAt = g_state.focusAt = -1;
    }
}

void* GameDisplay() { return g_gameDisplay.load(); }
unsigned long GameWindow() { return g_gameWindow.load(); }

namespace {
// (g_queryLock held.)
bool KeyDownLocked(Display* d, int vk)
{
    if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) {
        RefreshPointer(d);
        return (g_state.buttons & (vk == VK_LBUTTON ? kButton1Mask : vk == VK_RBUTTON ? kButton3Mask : kButton2Mask)) != 0;
    }
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) {  // (either side)
        const int left = vk == VK_SHIFT ? VK_LSHIFT : vk == VK_CONTROL ? VK_LCONTROL : VK_LMENU;
        return KeyDownLocked(d, left) || KeyDownLocked(d, left + 1);
    }
    if (vk <= 0 || vk > 255)
        return false;
    if (!g_state.codesKnown[vk]) {
        g_state.codesKnown[vk] = true;
        KeySym sym = 0, alt = 0;
        if (SymsFor(vk, sym, alt)) {
            g_state.codes[vk][0] = g_x.KeysymToKeycode(d, sym);
            g_state.codes[vk][1] = alt ? g_x.KeysymToKeycode(d, alt) : 0;
        }
    }
    RefreshKeys(d);
    return KeycodeDown(g_state.codes[vk][0]) || KeycodeDown(g_state.codes[vk][1]);
}
}  // namespace

bool KeyDown(int vk)
{
    std::lock_guard<std::mutex> lock(g_queryLock);
    Display* d = Query();
    return d && KeyDownLocked(d, vk);
}

bool PointerInWindow(int& x, int& y)
{
    std::lock_guard<std::mutex> lock(g_queryLock);
    Display* d = Query();
    if (!d || !g_gameWindow.load())
        return false;
    RefreshPointer(d);
    x = g_state.px;
    y = g_state.py;
    return g_state.pointerOk;
}

bool WindowSize(int& width, int& height)
{
    std::lock_guard<std::mutex> lock(g_queryLock);
    Display* d = Query();
    const Window w = g_gameWindow.load();
    if (!d || !w)
        return false;
    const double now = Now();
    if (now - g_state.geometryAt > 0.25) {
        g_state.geometryAt = now;
        Window root = 0;
        int x = 0, y = 0;
        unsigned border = 0, depth = 0;
        g_state.geometryOk = g_x.GetGeometry(d, w, &root, &x, &y, &g_state.width, &g_state.height, &border, &depth) != 0;
    }
    width = static_cast<int>(g_state.width);
    height = static_cast<int>(g_state.height);
    return g_state.geometryOk && width > 0 && height > 0;
}

// The game's window has the keyboard: the focus is it, or inside it, or it's inside the focus
// (a window manager's frame). Asked a few times a second.
bool WindowFocused()
{
    std::lock_guard<std::mutex> lock(g_queryLock);
    Display* d = Query();
    const Window w = g_gameWindow.load();
    if (!d || !w || !g_x.GetInputFocus)
        return true;
    const double now = Now();
    if (now - g_state.focusAt < 0.2)
        return g_state.focused;
    g_state.focusAt = now;
    Window focus = 0;
    int revert = 0;
    g_x.GetInputFocus(d, &focus, &revert);
    bool focused = focus == w || focus == 1 /* PointerRoot */;
    // Walk up from the focus (its parents) and from the window: either may hold the other.
    for (int pass = 0; pass < 2 && !focused && g_x.QueryTree && g_x.Free && focus > 1; ++pass) {
        Window from = pass == 0 ? focus : w, other = pass == 0 ? w : focus;
        for (int depth = 0; depth < 8 && from; ++depth) {
            Window root = 0, parent = 0, *children = nullptr;
            unsigned count = 0;
            if (!g_x.QueryTree(d, from, &root, &parent, &children, &count))
                break;
            if (children)
                g_x.Free(children);
            if (parent == other) {
                focused = true;
                break;
            }
            if (parent == root)
                break;
            from = parent;
        }
    }
    g_state.focused = focused;
    return focused;
}

// ---- The clipboard ----
namespace {
std::string Run(const char* command)
{
    std::string out;
    FILE* p = popen(command, "r");
    if (!p)
        return out;
    char buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof buffer, p)) > 0 && out.size() < (1u << 20))
        out.append(buffer, n);
    pclose(p);
    return out;
}

bool Have(const char* tool)
{
    char command[160];
    snprintf(command, sizeof command, "command -v %s >/dev/null 2>&1", tool);
    return system(command) == 0;
}
}  // namespace

bool ClipboardSet(const std::string& text)
{
    const char* command = nullptr;
    if (getenv("WAYLAND_DISPLAY") && Have("wl-copy"))
        command = "wl-copy 2>/dev/null";
    else if (Have("xclip"))
        command = "xclip -selection clipboard 2>/dev/null";
    else if (Have("xsel"))
        command = "xsel --clipboard --input 2>/dev/null";
    if (!command) {
        Log("clipboard: no wl-copy, xclip or xsel installed: install one to copy preset codes");
        return false;
    }
    FILE* p = popen(command, "w");
    if (!p)
        return false;
    const bool written = fwrite(text.data(), 1, text.size(), p) == text.size();
    return pclose(p) == 0 && written;
}

std::string ClipboardGet()
{
    if (getenv("WAYLAND_DISPLAY") && Have("wl-paste"))
        return Run("wl-paste --no-newline 2>/dev/null");
    if (Have("xclip"))
        return Run("xclip -selection clipboard -o 2>/dev/null");
    if (Have("xsel"))
        return Run("xsel --clipboard --output 2>/dev/null");
    Log("clipboard: no wl-paste, xclip or xsel installed: install one to paste preset codes");
    return std::string();
}

}  // namespace platform
}  // namespace mxcam

// ---- A key as the character it types, by the X keyboard layout ----
int ToUnicode(UINT vk, UINT, const BYTE* state, LPWSTR out, int size, UINT)
{
    if (!out || size < 1)
        return 0;
    std::lock_guard<std::mutex> lock(g_queryLock);
    Display* d = Query();
    if (!d || !g_x.KeycodeToKeysym)
        return 0;
    KeySym sym = 0, alt = 0;
    if (!SymsFor(static_cast<int>(vk), sym, alt))
        return 0;
    const KeyCode code = g_x.KeysymToKeycode(d, sym);
    if (!code)
        return 0;
    const bool shift = state && (state[VK_SHIFT] & 0x80);
    const bool caps = state && (state[VK_CAPITAL] & 0x01);
    KeySym typed = g_x.KeycodeToKeysym(d, code, 0, shift ? 1 : 0);
    if (caps && typed >= 'a' && typed <= 'z')
        typed -= 'a' - 'A';
    else if (caps && typed >= 'A' && typed <= 'Z')
        typed += 'a' - 'A';
    if (typed < 0x20 || typed > 0x7E)
        return 0;
    out[0] = static_cast<wchar_t>(typed);
    return 1;
}

// ---- The game's input events ----------------------------------------------------------------
namespace {
bool g_keyHeld[256] = {};

// An event on its way to the game: noted, and taken away from it while the menu has the input.
void Filter(Display* display, XEvent* event)
{
    if (!event)
        return;
    const int type = event->type;
    if (type == kKeyPress || type == kKeyRelease) {
        const unsigned code = event->xkey.keycode & 0xFF;
        const bool wasHeld = g_keyHeld[code];
        g_keyHeld[code] = type == kKeyPress;
        if (type == kKeyPress && g_x.LookupKeysym) {
            const int vk = VkOfSym(g_x.LookupKeysym(&event->xkey, 0));
            if (vk)
                mxcam::OverlayGameKeyDown(vk, wasHeld);
        }
    } else if (type == kButtonPress && (event->xbutton.button == 4 || event->xbutton.button == 5)) {
        if (mxcam::OverlayTakesInput())
            mxcam::OverlayWheel(event->xbutton.button == 4 ? 1 : -1);
    }
    if (event->xany.window && !g_gameWindow.load())
        mxcam::platform::NoteGameWindow(display, event->xany.window);
    if (mxcam::OverlayTakesInput() && (type == kKeyPress || type == kButtonPress || type == kMotionNotify)) {
        // (A property change of no property: nothing the game acts on.)
        const Window window = event->xany.window;
        const unsigned long serial = event->xany.serial;
        Display* from = event->xany.display;
        memset(event, 0, sizeof *event);
        event->xproperty.type = kPropertyNotify;
        event->xproperty.serial = serial;
        event->xproperty.display = from;
        event->xproperty.window = window;
        event->xproperty.atom = 0;
        event->xproperty.state = 1;  // PropertyDelete
    }
}
}  // namespace

#define MXCAM_EXPORT extern "C" __attribute__((visibility("default")))

MXCAM_EXPORT int XNextEvent(Display* display, XEvent* event)
{
    static auto real = Next<int (*)(Display*, XEvent*)>("XNextEvent");
    LoadX();
    const int r = real ? real(display, event) : 0;
    Filter(display, event);
    return r;
}

MXCAM_EXPORT int XIfEvent(Display* display, XEvent* event, PredicateFn predicate, char* arg)
{
    static auto real = Next<int (*)(Display*, XEvent*, PredicateFn, char*)>("XIfEvent");
    LoadX();
    const int r = real ? real(display, event, predicate, arg) : 0;
    Filter(display, event);
    return r;
}

MXCAM_EXPORT Bool XCheckIfEvent(Display* display, XEvent* event, PredicateFn predicate, char* arg)
{
    static auto real = Next<Bool (*)(Display*, XEvent*, PredicateFn, char*)>("XCheckIfEvent");
    LoadX();
    const Bool r = real ? real(display, event, predicate, arg) : 0;
    if (r)
        Filter(display, event);
    return r;
}

MXCAM_EXPORT int XWindowEvent(Display* display, Window window, long mask, XEvent* event)
{
    static auto real = Next<int (*)(Display*, Window, long, XEvent*)>("XWindowEvent");
    LoadX();
    const int r = real ? real(display, window, mask, event) : 0;
    Filter(display, event);
    return r;
}

MXCAM_EXPORT Bool XCheckWindowEvent(Display* display, Window window, long mask, XEvent* event)
{
    static auto real = Next<Bool (*)(Display*, Window, long, XEvent*)>("XCheckWindowEvent");
    LoadX();
    const Bool r = real ? real(display, window, mask, event) : 0;
    if (r)
        Filter(display, event);
    return r;
}

MXCAM_EXPORT int XMaskEvent(Display* display, long mask, XEvent* event)
{
    static auto real = Next<int (*)(Display*, long, XEvent*)>("XMaskEvent");
    LoadX();
    const int r = real ? real(display, mask, event) : 0;
    Filter(display, event);
    return r;
}

MXCAM_EXPORT Bool XCheckMaskEvent(Display* display, long mask, XEvent* event)
{
    static auto real = Next<Bool (*)(Display*, long, XEvent*)>("XCheckMaskEvent");
    LoadX();
    const Bool r = real ? real(display, mask, event) : 0;
    if (r)
        Filter(display, event);
    return r;
}

MXCAM_EXPORT Bool XCheckTypedEvent(Display* display, int type, XEvent* event)
{
    static auto real = Next<Bool (*)(Display*, int, XEvent*)>("XCheckTypedEvent");
    LoadX();
    const Bool r = real ? real(display, type, event) : 0;
    if (r)
        Filter(display, event);
    return r;
}

MXCAM_EXPORT Bool XCheckTypedWindowEvent(Display* display, Window window, int type, XEvent* event)
{
    static auto real = Next<Bool (*)(Display*, Window, int, XEvent*)>("XCheckTypedWindowEvent");
    LoadX();
    const Bool r = real ? real(display, window, type, event) : 0;
    if (r)
        Filter(display, event);
    return r;
}
