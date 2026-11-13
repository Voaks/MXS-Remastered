// hooks.cpp
//   The intercepted OpenGL entry points.
//
//   How the camera is found
//   -----------------------
//   We mirror just enough of the fixed-function matrix state to know, at every modelview
//   load, whether a perspective projection is active (3D scene) or not (HUD / menus / 2D).
//   Each frame, the modelview loads made during perspective passes are numbered 0, 1, 2...
//   Load number ViewMatrixIndex (MXS_REMASTERED.ini) is taken to be the camera's view matrix. Use the
//   frame dump (F10) to see what the game actually does and pick the right index.
//
//   How it is modified
//   ------------------
//   camera::Update() turns that raw view into an eye-space correction C. Every later
//   perspective-pass modelview load that frame is replaced with C * M - correct whether the
//   engine loads the bare view (then glMultMatrix's objects onto it) or loads a full
//   View*Model per object, because C * View * Model == NewView * Model.
//
//   A "load" is glLoadMatrixf/d, or glMultMatrixf/d onto a freshly loaded identity (which
//   is equivalent, and is how glLoadIdentity(); glMultMatrixf(view); engines set a camera).

#include <windows.h>
#include <GL/gl.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#ifndef _WIN32
#include <dlfcn.h>
#include "linux/platform_linux.h"
#endif

#include "camera.h"
#include "config.h"
#include "hooks.h"
#include "log.h"
#include "mathlib.h"
#include "overlay.h"
#include "postfx.h"
#include "texfilter.h"

namespace mxcam {
namespace {

// ===========================================================================
// Real driver entry points, filled in by the proxy loader before any game code runs.
// decltype(&::glFoo) takes the exact signature and calling convention from <GL/gl.h>
// without importing the symbol.
// ===========================================================================
#ifdef _WIN32
using PFN_wglSwapBuffers = BOOL(WINAPI*)(HDC);  // exported by opengl32 but not declared in the SDK
#else
// Linux: the frame ends at the GLX (X11) or EGL buffer swap; newer functions come through their
// get-proc-address (the game uses GLX; EGL is covered too).
using PFN_glXSwapBuffers     = void (*)(void* display, unsigned long drawable);
using PFN_glXGetProcAddress  = void (*(*)(const GLubyte* name))();
using PFN_eglSwapBuffers     = unsigned (*)(void* display, void* surface);
using PFN_eglGetProcAddress  = void (*(*)(const char* name))();
#endif

decltype(&::glMatrixMode)        real_glMatrixMode        = nullptr;
decltype(&::glLoadIdentity)      real_glLoadIdentity      = nullptr;
decltype(&::glLoadMatrixf)       real_glLoadMatrixf       = nullptr;
decltype(&::glLoadMatrixd)       real_glLoadMatrixd       = nullptr;
decltype(&::glMultMatrixf)       real_glMultMatrixf       = nullptr;
decltype(&::glMultMatrixd)       real_glMultMatrixd       = nullptr;
decltype(&::glPushMatrix)        real_glPushMatrix        = nullptr;
decltype(&::glPopMatrix)         real_glPopMatrix         = nullptr;
decltype(&::glFrustum)           real_glFrustum           = nullptr;
decltype(&::glOrtho)             real_glOrtho             = nullptr;
decltype(&::glRotatef)           real_glRotatef           = nullptr;
decltype(&::glRotated)           real_glRotated           = nullptr;
decltype(&::glTranslatef)        real_glTranslatef        = nullptr;
decltype(&::glTranslated)        real_glTranslated        = nullptr;
decltype(&::glScalef)            real_glScalef            = nullptr;
decltype(&::glScaled)            real_glScaled            = nullptr;
decltype(&::glNewList)           real_glNewList           = nullptr;
decltype(&::glEndList)           real_glEndList           = nullptr;
decltype(&::glClear)             real_glClear             = nullptr;
decltype(&::glDrawElements)      real_glDrawElements      = nullptr;
decltype(&::glDrawArrays)        real_glDrawArrays        = nullptr;
decltype(&::glTexParameteri)     real_glTexParameteri     = nullptr;
decltype(&::glTexParameterf)     real_glTexParameterf     = nullptr;
decltype(&::glTexParameteriv)    real_glTexParameteriv    = nullptr;
decltype(&::glTexParameterfv)    real_glTexParameterfv    = nullptr;
decltype(&::glDeleteTextures)    real_glDeleteTextures    = nullptr;
decltype(&::glBindTexture)       real_glBindTexture       = nullptr;
decltype(&::glDepthMask)         real_glDepthMask         = nullptr;
decltype(&::glEnable)            real_glEnable            = nullptr;
decltype(&::glDisable)           real_glDisable           = nullptr;
#ifdef _WIN32
PFN_wglSwapBuffers               real_wglSwapBuffers      = nullptr;
decltype(&::wglSwapLayerBuffers) real_wglSwapLayerBuffers = nullptr;
decltype(&::wglGetProcAddress)   real_wglGetProcAddress   = nullptr;
#else
PFN_glXSwapBuffers               real_glXSwapBuffers         = nullptr;
PFN_glXGetProcAddress            real_glXGetProcAddress      = nullptr;
PFN_glXGetProcAddress            real_glXGetProcAddressARB   = nullptr;
PFN_eglSwapBuffers               real_eglSwapBuffers         = nullptr;
PFN_eglGetProcAddress            real_eglGetProcAddress      = nullptr;
#endif

// ===========================================================================
// Mirrored GL state
// ===========================================================================
constexpr int kStackDepth   = 64;    // >= any driver's modelview / projection stack depth
constexpr int kMaxDumpLines = 4000;  // per frame dump

struct MatrixState {
    GLenum mode     = GL_MODELVIEW;
    GLenum listMode = 0;             // GL_COMPILE / GL_COMPILE_AND_EXECUTE inside glNewList..glEndList
    bool   projIsPerspective[kStackDepth] = {};
    int    projTop = 0;
    bool   mvIsIdentity[kStackDepth] = {};  // "top of modelview is a fresh glLoadIdentity"
    int    mvTop = 0;
};

struct FrameState {
    int    sceneLoadIndex = 0;       // modelview loads seen this frame during perspective passes
    bool   captured       = false;   // the camera load was reached this frame
    bool   haveCorrection = false;   // ...and camera::Update() accepted it
    double correction[16] = {};
    double lastOutput[16] = {};      // the last matrix we substituted (see MatchesLastOutput)
    bool   haveLastOutput = false;
    bool   dumping        = false;
    int    dumpLines      = 0;
};

MatrixState g_gl;
FrameState  g_frame;
bool        g_dumpRequested       = false;
int         g_framesWithoutCamera = 0;
ULONGLONG   g_lastSwapTick        = 0;

// GL_COMPILE records commands into a display list without executing them, so they must not
// change our mirror of the live state.
bool Executes() { return g_gl.listMode != GL_COMPILE; }
bool& ProjIsPerspective() { return g_gl.projIsPerspective[g_gl.projTop]; }
bool& MvIsIdentity() { return g_gl.mvIsIdentity[g_gl.mvTop]; }

// A perspective projection makes clip-space w depend on z (m[11] != 0) with no constant
// w term (m[15] == 0); orthographic and identity projections have m[15] == 1.
template <class T>
bool IsPerspective(const T* m)
{
    return std::fabs(double(m[15])) < 1e-6 && std::fabs(double(m[11])) > 1e-6;
}

// A perspective projection, for the shaders' depth reconstruction (postfx.h).
template <class T>
void NoteProjection(const T* m)
{
    float f[16];
    for (int i = 0; i < 16; ++i)
        f[i] = static_cast<float>(m[i]);
    postfx::NoteProjection(f, true);
}

void LoadReal(const GLfloat* m)  { real_glLoadMatrixf(m); }
void LoadReal(const GLdouble* m) { real_glLoadMatrixd(m); }
void MultReal(const GLfloat* m)  { real_glMultMatrixf(m); }
void MultReal(const GLdouble* m) { real_glMultMatrixd(m); }

// ===========================================================================
// Frame dump (diagnostics): logs every matrix call of one frame
// ===========================================================================
const char* ModeName(GLenum mode)
{
    switch (mode) {
    case GL_MODELVIEW:  return "MODELVIEW";
    case GL_PROJECTION: return "PROJECTION";
    case GL_TEXTURE:    return "TEXTURE";
    default:            return "other";
    }
}

}  // namespace

bool IsDumping() { return g_frame.dumping && g_frame.dumpLines < kMaxDumpLines; }

#ifdef _WIN32
bool GameHasFocus()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

void* GetRealGLFunction(const char* name)
{
    if (void* exported = reinterpret_cast<void*>(GetProcAddress(RealOpenGLModule(), name)))
        return exported;
    if (!real_wglGetProcAddress)
        return nullptr;
    // Some drivers return small sentinel values instead of NULL for unsupported functions.
    const auto address = reinterpret_cast<INT_PTR>(real_wglGetProcAddress(name));
    return (address >= -1 && address <= 3) ? nullptr : reinterpret_cast<void*>(address);
}
#else
bool GameHasFocus() { return platform::WindowFocused(); }

// The real function: the next library's (libGL's: this one is preloaded ahead of it), or through
// the real glXGetProcAddress for what libGL doesn't export. The Windows-only names the shared code
// asks for are given their GLX stand-ins (win32_compat.cpp).
void* GetRealGLFunction(const char* name)
{
    if (strcmp(name, "wglGetCurrentContext") == 0)
        return reinterpret_cast<void*>(&::wglGetCurrentContext);
    if (strcmp(name, "wglGetCurrentDC") == 0)
        return reinterpret_cast<void*>(&::wglGetCurrentDC);
    if (void* next = dlsym(RTLD_NEXT, name))
        return next;
    if (!real_glXGetProcAddressARB)
        real_glXGetProcAddressARB = reinterpret_cast<PFN_glXGetProcAddress>(dlsym(RTLD_NEXT, "glXGetProcAddressARB"));
    if (real_glXGetProcAddressARB)
        return reinterpret_cast<void*>(real_glXGetProcAddressARB(reinterpret_cast<const GLubyte*>(name)));
    return nullptr;
}
#endif

void DumpCall(const char* format, ...)
{
    if (!g_frame.dumping || g_frame.dumpLines++ >= kMaxDumpLines)
        return;
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    Log("  %-10s %-5s %s", ModeName(g_gl.mode), ProjIsPerspective() ? "3D" : "2D", text);
}

namespace {

template <class T>
void DumpMatrix(const char* call, const T* m)
{
    // Printed row by row; the 4th column is the translation.
    DumpCall("%s [% .3f % .3f % .3f % .3f | % .3f % .3f % .3f % .3f | % .3f % .3f % .3f % .3f | % .3f % .3f % .3f % .3f]",
             call, m[0], m[4], m[8], m[12], m[1], m[5], m[9], m[13],
             m[2], m[6], m[10], m[14], m[3], m[7], m[11], m[15]);
}

// ===========================================================================
// Camera capture & correction
// ===========================================================================

// Some engines read the modelview back (glGetFloatv) and load it again later. If that
// matrix is one we produced, it is already corrected and must not be corrected twice.
template <class T>
bool MatchesLastOutput(const T* m)
{
    if (!g_frame.haveLastOutput)
        return false;
    for (int i = 0; i < 16; ++i) {
        const double expected = g_frame.lastOutput[i];
        if (std::fabs(double(m[i]) - expected) > 1e-5 * (1.0 + std::fabs(expected)))
            return false;
    }
    return true;
}

// The engine is about to replace the whole modelview matrix with m.
template <class T>
void SetModelview(const T* m)
{
    const Config& cfg = GetConfig();
    if (!cfg.enabled || !ProjIsPerspective() || g_gl.listMode != 0) {
        LoadReal(m);
        return;
    }

    const int index = g_frame.sceneLoadIndex++;
    if (index == cfg.viewMatrixIndex) {
        double view[16];
        for (int i = 0; i < 16; ++i)
            view[i] = double(m[i]);
        g_frame.captured = true;
        g_frame.haveCorrection = camera::Update(view, g_frame.correction);
        double rendered[16];
        if (g_frame.haveCorrection)
            MatMul(g_frame.correction, view, rendered);
        postfx::NoteView(view, g_frame.haveCorrection ? rendered : view);
        if (g_frame.haveCorrection) {
            const camera::Telemetry& t = camera::LastTelemetry();
            DumpCall("^ scene load #%d = CAMERA  dt=%.1fms lag=%.2fdeg roll=%.2fdeg rollCorr=%.2fdeg "
                     "accelUp=%.2fg neck=%.1fmm eyeHeight%+.1fmm",
                     index, t.dtMs, t.lagDeg, t.rawRollDeg, t.rollCorrectionDeg, t.verticalAccelG,
                     t.neckOffset * 1000.0, t.verticalOffset * 1000.0);
        } else {
            DumpCall("^ scene load #%d = CAMERA, rejected (not a rigid view matrix)", index);
        }
    } else {
        DumpCall("^ scene load #%d", index);
    }

    if (!g_frame.haveCorrection || MatchesLastOutput(m)) {
        LoadReal(m);
        return;
    }

    // out = C * m, accumulated in double precision.
    T out[16];
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            double sum = 0;
            for (int k = 0; k < 4; ++k)
                sum += g_frame.correction[k * 4 + r] * double(m[c * 4 + k]);
            out[c * 4 + r] = T(sum);
            g_frame.lastOutput[c * 4 + r] = double(out[c * 4 + r]);
        }
    }
    g_frame.haveLastOutput = true;
    LoadReal(out);
}

// ===========================================================================
// Frame boundary & hotkeys
// ===========================================================================
struct HotkeyState {
    int  vk = 0;
    bool down = false;
};

bool Pressed(int vk, HotkeyState& key)
{
    const bool down = vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
    // Just rebound in the panel: the press that picked the key doesn't count.
    const bool pressed = down && !key.down && vk == key.vk;
    key.vk = vk;
    key.down = down;
    return pressed;
}

void PollHotkeys()
{
    static HotkeyState toggleKey, reloadKey, dumpKey, traceKey, shadersKey;
    const Config& cfg = GetConfig();
    // Always sample all keys so the edge detection stays in sync while unfocused.
    const bool toggle = Pressed(cfg.toggleKey, toggleKey);
    const bool reload = Pressed(cfg.reloadKey, reloadKey);
    const bool dump   = Pressed(cfg.dumpKey, dumpKey);
    const bool trace  = Pressed(cfg.traceKey, traceKey);
    const bool shaders = Pressed(cfg.shaderToggleKey, shadersKey);
    // For automated tests: the ini read again on a given frame (a setting changed while playing).
    static const int testReloadFrame = [] {
        char text[16] = "";
        return GetEnvironmentVariableA("MXCAM_TEST_RELOAD_FRAME", text, sizeof text) > 0 ? atoi(text) : -1;
    }();
    static int polledFrames = 0;
    if (testReloadFrame >= 0 && polledFrames++ == testReloadFrame)
        ReloadConfig();
    if (!GameHasFocus() || OverlayListeningForKey())  // a keybind is being picked: that press is its answer
        return;

    if (toggle) {
        SetEnabled(!cfg.enabled);
        camera::Reset();
        Log("hotkey: helmet cam %s", cfg.enabled ? "ON" : "OFF");
    }
    if (reload) {
        ReloadConfig();
        camera::Reset();
    }
    if (dump)
        g_dumpRequested = true;
    if (trace)
        StartTrace(5.0);
    if (shaders) {
        EditConfig().shaders = !cfg.shaders;
        Log("hotkey: shaders %s", cfg.shaders ? "ON" : "OFF");
    }
}

void EndFrame()
{
    const Config& cfg = GetConfig();
    const bool shaderCaptured = EndShaderFrame();
    if (g_frame.dumping)
        Log("==== end of frame dump: %d scene modelview loads ====", g_frame.sceneLoadIndex);

    if (cfg.enabled && !g_frame.captured && !shaderCaptured) {
        if (++g_framesWithoutCamera == 600) {
            Log("note: no camera matrix found for 600 frames (menu, replay, or detection settings?). "
                "Press the dump key in first person to inspect a frame.");
        }
    } else {
        g_framesWithoutCamera = 0;
    }

    g_frame = FrameState{};
    PollHotkeys();
    if (g_dumpRequested) {
        g_dumpRequested = false;
        g_frame.dumping = true;
        Log("==== frame dump begin (ViewMatrixIndex=%d) ====", cfg.viewMatrixIndex);
    }
}

// ===========================================================================
// Hooks
// ===========================================================================
void APIENTRY Hook_glMatrixMode(GLenum mode)
{
    if (Executes())
        g_gl.mode = mode;
    DumpCall("glMatrixMode(%s)", ModeName(mode));
    real_glMatrixMode(mode);
}

void APIENTRY Hook_glLoadIdentity()
{
    if (Executes()) {
        if (g_gl.mode == GL_MODELVIEW)
            MvIsIdentity() = true;
        else if (g_gl.mode == GL_PROJECTION)
            ProjIsPerspective() = false;
    }
    DumpCall("glLoadIdentity()");
    real_glLoadIdentity();
}

template <class T>
void OnLoadMatrix(const T* m, const char* name)
{
    if (Executes()) {
        if (g_gl.mode == GL_PROJECTION)
            ProjIsPerspective() = IsPerspective(m);
        else if (g_gl.mode == GL_MODELVIEW)
            MvIsIdentity() = false;
        if (g_gl.mode == GL_PROJECTION && IsPerspective(m))
            NoteProjection(m);
    }
    DumpMatrix(name, m);
    if (g_gl.mode == GL_MODELVIEW && g_gl.listMode == 0)
        SetModelview(m);
    else
        LoadReal(m);
}

template <class T>
void OnMultMatrix(const T* m, const char* name)
{
    bool ontoIdentity = false;
    if (Executes()) {
        if (g_gl.mode == GL_PROJECTION) {
            if (IsPerspective(m)) {
                ProjIsPerspective() = true;
                NoteProjection(m);  // gluPerspective: a multiply onto a fresh identity
            }
        } else if (g_gl.mode == GL_MODELVIEW) {
            ontoIdentity = MvIsIdentity();
            MvIsIdentity() = false;
        }
    }
    DumpMatrix(name, m);
    // Identity * M == M: a multiply onto a fresh identity is really a load.
    if (ontoIdentity && g_gl.listMode == 0)
        SetModelview(m);
    else
        MultReal(m);
}

void APIENTRY Hook_glLoadMatrixf(const GLfloat* m)  { OnLoadMatrix(m, "glLoadMatrixf"); }
void APIENTRY Hook_glLoadMatrixd(const GLdouble* m) { OnLoadMatrix(m, "glLoadMatrixd"); }
void APIENTRY Hook_glMultMatrixf(const GLfloat* m)  { OnMultMatrix(m, "glMultMatrixf"); }
void APIENTRY Hook_glMultMatrixd(const GLdouble* m) { OnMultMatrix(m, "glMultMatrixd"); }

void APIENTRY Hook_glPushMatrix()
{
    if (Executes()) {
        if (g_gl.mode == GL_PROJECTION && g_gl.projTop + 1 < kStackDepth) {
            g_gl.projIsPerspective[g_gl.projTop + 1] = ProjIsPerspective();
            ++g_gl.projTop;
        } else if (g_gl.mode == GL_MODELVIEW && g_gl.mvTop + 1 < kStackDepth) {
            g_gl.mvIsIdentity[g_gl.mvTop + 1] = MvIsIdentity();
            ++g_gl.mvTop;
        }
    }
    DumpCall("glPushMatrix()");
    real_glPushMatrix();
}

void APIENTRY Hook_glPopMatrix()
{
    if (Executes()) {
        if (g_gl.mode == GL_PROJECTION && g_gl.projTop > 0)
            --g_gl.projTop;
        else if (g_gl.mode == GL_MODELVIEW && g_gl.mvTop > 0)
            --g_gl.mvTop;
    }
    DumpCall("glPopMatrix()");
    real_glPopMatrix();
}

void APIENTRY Hook_glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    if (Executes() && g_gl.mode == GL_PROJECTION) {
        ProjIsPerspective() = true;
        if (r != l && t != b && f != n) {  // glFrustum's matrix, for the shaders' depth
            const double m[16] = { 2 * n / (r - l), 0, 0, 0,  0, 2 * n / (t - b), 0, 0,
                                   (r + l) / (r - l), (t + b) / (t - b), -(f + n) / (f - n), -1,
                                   0, 0, -2 * f * n / (f - n), 0 };
            NoteProjection(m);
        }
    }
    DumpCall("glFrustum(%g, %g, %g, %g, %g, %g)", l, r, b, t, n, f);
    real_glFrustum(l, r, b, t, n, f);
}

void APIENTRY Hook_glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    DumpCall("glOrtho(%g, %g, %g, %g, %g, %g)", l, r, b, t, n, f);
    real_glOrtho(l, r, b, t, n, f);
}

// Rotate / translate / scale multiply onto the stack, so its top is no longer a plain identity.
void NoteModelviewMultiplied()
{
    if (Executes() && g_gl.mode == GL_MODELVIEW)
        MvIsIdentity() = false;
}

void APIENTRY Hook_glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z)
{
    NoteModelviewMultiplied();
    DumpCall("glRotatef(%g, %g, %g, %g)", a, x, y, z);
    real_glRotatef(a, x, y, z);
}

void APIENTRY Hook_glRotated(GLdouble a, GLdouble x, GLdouble y, GLdouble z)
{
    NoteModelviewMultiplied();
    DumpCall("glRotated(%g, %g, %g, %g)", a, x, y, z);
    real_glRotated(a, x, y, z);
}

void APIENTRY Hook_glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
    NoteModelviewMultiplied();
    DumpCall("glTranslatef(%g, %g, %g)", x, y, z);
    real_glTranslatef(x, y, z);
}

void APIENTRY Hook_glTranslated(GLdouble x, GLdouble y, GLdouble z)
{
    NoteModelviewMultiplied();
    DumpCall("glTranslated(%g, %g, %g)", x, y, z);
    real_glTranslated(x, y, z);
}

void APIENTRY Hook_glScalef(GLfloat x, GLfloat y, GLfloat z)
{
    NoteModelviewMultiplied();
    DumpCall("glScalef(%g, %g, %g)", x, y, z);
    real_glScalef(x, y, z);
}

void APIENTRY Hook_glScaled(GLdouble x, GLdouble y, GLdouble z)
{
    NoteModelviewMultiplied();
    DumpCall("glScaled(%g, %g, %g)", x, y, z);
    real_glScaled(x, y, z);
}

// Matrix commands recorded into a display list are left untouched: a corrected matrix baked
// into a list would replay this frame's correction forever.
void APIENTRY Hook_glNewList(GLuint list, GLenum mode)
{
    g_gl.listMode = mode;
    DumpCall("glNewList(%u, 0x%X)", list, mode);
    real_glNewList(list, mode);
}

void APIENTRY Hook_glEndList()
{
    g_gl.listMode = 0;
    DumpCall("glEndList()");
    real_glEndList();
}

// Fallback frame boundary for games whose buffer swap bypasses opengl32!wglSwapBuffers:
// if no swap has been seen for a second, a colour clear after the camera starts a new frame.
void APIENTRY Hook_glClear(GLbitfield mask)
{
    if ((mask & GL_COLOR_BUFFER_BIT) && g_frame.captured && GetTickCount64() - g_lastSwapTick > 1000) {
        postfx::FrameEnd(false);
        EndFrame();
    }
    DumpCall("glClear(0x%X)", mask);
    real_glClear(mask);
}

void APIENTRY Hook_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices)
{
    DrawCall draw;
    draw.kind = DrawCall::Elements;
    draw.mode = mode;
    draw.count = count;
    draw.type = type;
    draw.indices = indices;
    if (NoteDraw(draw))
        real_glDrawElements(mode, count, type, indices);
}

void APIENTRY Hook_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    DrawCall draw;
    draw.kind = DrawCall::Arrays;
    draw.mode = mode;
    draw.first = first;
    draw.count = count;
    if (NoteDraw(draw))
        real_glDrawArrays(mode, first, count);
}

// Texture filtering (texfilter.h): the game's filter settings, adjusted on the way to the driver.
void APIENTRY Hook_glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    if (texfilter::Watches(pname))
        param = static_cast<GLint>(texfilter::TextureParameter(target, pname, static_cast<float>(param)));
    real_glTexParameteri(target, pname, param);
}

void APIENTRY Hook_glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{
    if (texfilter::Watches(pname))
        param = texfilter::TextureParameter(target, pname, param);
    real_glTexParameterf(target, pname, param);
}

void APIENTRY Hook_glTexParameteriv(GLenum target, GLenum pname, const GLint* params)
{
    if (params && texfilter::Watches(pname)) {
        const GLint value = static_cast<GLint>(texfilter::TextureParameter(target, pname, static_cast<float>(params[0])));
        real_glTexParameteriv(target, pname, &value);
        return;
    }
    real_glTexParameteriv(target, pname, params);
}

void APIENTRY Hook_glTexParameterfv(GLenum target, GLenum pname, const GLfloat* params)
{
    if (params && texfilter::Watches(pname)) {
        const GLfloat value = texfilter::TextureParameter(target, pname, params[0]);
        real_glTexParameterfv(target, pname, &value);
        return;
    }
    real_glTexParameterfv(target, pname, params);
}

void APIENTRY Hook_glDepthMask(GLboolean flag)
{
    NoteDepthMask(flag != GL_FALSE);
    real_glDepthMask(flag);
}

void APIENTRY Hook_glEnable(GLenum capability)
{
    NoteCapability(capability, true);
    real_glEnable(capability);
}

void APIENTRY Hook_glDisable(GLenum capability)
{
    NoteCapability(capability, false);
    real_glDisable(capability);
}

void APIENTRY Hook_glBindTexture(GLenum target, GLuint texture)
{
    NoteBindTexture(target, texture);
    real_glBindTexture(target, texture);
}

void APIENTRY Hook_glDeleteTextures(GLsizei n, const GLuint* textures)
{
    texfilter::TexturesDeleted(n, textures);
    real_glDeleteTextures(n, textures);
}

// The shaders run on the finished 3D image (if the HUD start wasn't recognised earlier), then
// the settings panel is drawn into the frame, right before it is presented.
#ifdef _WIN32
BOOL WINAPI Hook_wglSwapBuffers(HDC dc)
{
    postfx::FrameEnd();
    OverlayFrame(dc);
    texfilter::FrameEnd();
    g_lastSwapTick = GetTickCount64();
    EndFrame();
    return real_wglSwapBuffers(dc);
}

BOOL WINAPI Hook_wglSwapLayerBuffers(HDC dc, UINT planes)
{
    if (planes & WGL_SWAP_MAIN_PLANE) {
        postfx::FrameEnd();
        OverlayFrame(dc);
        texfilter::FrameEnd();
        g_lastSwapTick = GetTickCount64();
        EndFrame();
    }
    return real_wglSwapLayerBuffers(dc, planes);
}

// Everything past OpenGL 1.1 (shaders, uniforms, buffers) is fetched through here.
PROC WINAPI Hook_wglGetProcAddress(LPCSTR name)
{
    const PROC real = real_wglGetProcAddress(name);
    return reinterpret_cast<PROC>(WrapProc(name, reinterpret_cast<void*>(real)));
}
#else
// The frame's end, as wglSwapBuffers is on Windows: the game's window and display are noted (the
// menu reads its input and size from them), then the same as there.
void EndOfFrame()
{
    postfx::FrameEnd();
    OverlayFrame(wglGetCurrentDC());
    texfilter::FrameEnd();
    g_lastSwapTick = GetTickCount64();
    EndFrame();
}

void Hook_glXSwapBuffers(void* display, unsigned long drawable)
{
    platform::NoteGameWindow(display, drawable);
    EndOfFrame();
    real_glXSwapBuffers(display, drawable);
}

unsigned Hook_eglSwapBuffers(void* display, void* surface)
{
    EndOfFrame();
    return real_eglSwapBuffers(display, surface);
}

// Everything past OpenGL 1.1 (shaders, uniforms, buffers) is fetched through here.
void (*Hook_glXGetProcAddress(const GLubyte* name))()
{
    void (*real)() = real_glXGetProcAddress ? real_glXGetProcAddress(name) : nullptr;
    return reinterpret_cast<void (*)()>(WrapProc(reinterpret_cast<const char*>(name), reinterpret_cast<void*>(real)));
}

void (*Hook_glXGetProcAddressARB(const GLubyte* name))()
{
    void (*real)() = real_glXGetProcAddressARB ? real_glXGetProcAddressARB(name) : nullptr;
    return reinterpret_cast<void (*)()>(WrapProc(reinterpret_cast<const char*>(name), reinterpret_cast<void*>(real)));
}

void (*Hook_eglGetProcAddress(const char* name))()
{
    void (*real)() = real_eglGetProcAddress ? real_eglGetProcAddress(name) : nullptr;
    return reinterpret_cast<void (*)()>(WrapProc(name, reinterpret_cast<void*>(real)));
}
#endif

}  // namespace

#define MXCAM_HOOK(fn) { #fn, reinterpret_cast<void*>(&Hook_##fn), reinterpret_cast<void**>(&real_##fn) }

const HookEntry* GetHookTable(size_t& count)
{
    static const HookEntry kHooks[] = {
        MXCAM_HOOK(glMatrixMode),   MXCAM_HOOK(glLoadIdentity),
        MXCAM_HOOK(glLoadMatrixf),  MXCAM_HOOK(glLoadMatrixd),
        MXCAM_HOOK(glMultMatrixf),  MXCAM_HOOK(glMultMatrixd),
        MXCAM_HOOK(glPushMatrix),   MXCAM_HOOK(glPopMatrix),
        MXCAM_HOOK(glFrustum),      MXCAM_HOOK(glOrtho),
        MXCAM_HOOK(glRotatef),      MXCAM_HOOK(glRotated),
        MXCAM_HOOK(glTranslatef),   MXCAM_HOOK(glTranslated),
        MXCAM_HOOK(glScalef),       MXCAM_HOOK(glScaled),
        MXCAM_HOOK(glNewList),      MXCAM_HOOK(glEndList),
        MXCAM_HOOK(glClear),
        MXCAM_HOOK(glDrawElements), MXCAM_HOOK(glDrawArrays),
        MXCAM_HOOK(glTexParameteri),  MXCAM_HOOK(glTexParameterf),
        MXCAM_HOOK(glTexParameteriv), MXCAM_HOOK(glTexParameterfv),
        MXCAM_HOOK(glDeleteTextures), MXCAM_HOOK(glBindTexture),
        MXCAM_HOOK(glDepthMask),    MXCAM_HOOK(glEnable),      MXCAM_HOOK(glDisable),
#ifdef _WIN32
        MXCAM_HOOK(wglSwapBuffers), MXCAM_HOOK(wglSwapLayerBuffers),
        MXCAM_HOOK(wglGetProcAddress),
#else
        MXCAM_HOOK(glXSwapBuffers), MXCAM_HOOK(glXGetProcAddress), MXCAM_HOOK(glXGetProcAddressARB),
        MXCAM_HOOK(eglSwapBuffers), MXCAM_HOOK(eglGetProcAddress),
#endif
    };
    count = _countof(kHooks);
    return kHooks;
}

#ifndef _WIN32
// Each hooked function's real one, from the library after this one (libGL, libEGL): set when this
// library loads (linux/entry_linux.cpp), and again if one is called before then.
void ResolveRealFunctions()
{
    size_t count = 0;
    const HookEntry* hooks = GetHookTable(count);
    for (size_t i = 0; i < count; ++i)
        if (!*hooks[i].realSlot)
            *hooks[i].realSlot = dlsym(RTLD_NEXT, hooks[i].name);
}
#endif

}  // namespace mxcam

#ifndef _WIN32
// ---- What the game calls (Linux) ------------------------------------------------------------
// This library is preloaded (LD_PRELOAD) ahead of libGL: the game's calls to these come here,
// and each goes on to the real one, as the export stubs do on Windows (proxy.cpp).
#define MXCAM_EXPORT extern "C" __attribute__((visibility("default")))
#define MXCAM_REAL(fn) (mxcam::real_##fn ? 0 : (mxcam::ResolveRealFunctions(), 0))

MXCAM_EXPORT void glMatrixMode(GLenum mode) { MXCAM_REAL(glMatrixMode); mxcam::Hook_glMatrixMode(mode); }
MXCAM_EXPORT void glLoadIdentity() { MXCAM_REAL(glLoadIdentity); mxcam::Hook_glLoadIdentity(); }
MXCAM_EXPORT void glLoadMatrixf(const GLfloat* m) { MXCAM_REAL(glLoadMatrixf); mxcam::Hook_glLoadMatrixf(m); }
MXCAM_EXPORT void glLoadMatrixd(const GLdouble* m) { MXCAM_REAL(glLoadMatrixd); mxcam::Hook_glLoadMatrixd(m); }
MXCAM_EXPORT void glMultMatrixf(const GLfloat* m) { MXCAM_REAL(glMultMatrixf); mxcam::Hook_glMultMatrixf(m); }
MXCAM_EXPORT void glMultMatrixd(const GLdouble* m) { MXCAM_REAL(glMultMatrixd); mxcam::Hook_glMultMatrixd(m); }
MXCAM_EXPORT void glPushMatrix() { MXCAM_REAL(glPushMatrix); mxcam::Hook_glPushMatrix(); }
MXCAM_EXPORT void glPopMatrix() { MXCAM_REAL(glPopMatrix); mxcam::Hook_glPopMatrix(); }
MXCAM_EXPORT void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    MXCAM_REAL(glFrustum);
    mxcam::Hook_glFrustum(l, r, b, t, n, f);
}
MXCAM_EXPORT void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    MXCAM_REAL(glOrtho);
    mxcam::Hook_glOrtho(l, r, b, t, n, f);
}
MXCAM_EXPORT void glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z) { MXCAM_REAL(glRotatef); mxcam::Hook_glRotatef(a, x, y, z); }
MXCAM_EXPORT void glRotated(GLdouble a, GLdouble x, GLdouble y, GLdouble z) { MXCAM_REAL(glRotated); mxcam::Hook_glRotated(a, x, y, z); }
MXCAM_EXPORT void glTranslatef(GLfloat x, GLfloat y, GLfloat z) { MXCAM_REAL(glTranslatef); mxcam::Hook_glTranslatef(x, y, z); }
MXCAM_EXPORT void glTranslated(GLdouble x, GLdouble y, GLdouble z) { MXCAM_REAL(glTranslated); mxcam::Hook_glTranslated(x, y, z); }
MXCAM_EXPORT void glScalef(GLfloat x, GLfloat y, GLfloat z) { MXCAM_REAL(glScalef); mxcam::Hook_glScalef(x, y, z); }
MXCAM_EXPORT void glScaled(GLdouble x, GLdouble y, GLdouble z) { MXCAM_REAL(glScaled); mxcam::Hook_glScaled(x, y, z); }
MXCAM_EXPORT void glNewList(GLuint list, GLenum mode) { MXCAM_REAL(glNewList); mxcam::Hook_glNewList(list, mode); }
MXCAM_EXPORT void glEndList() { MXCAM_REAL(glEndList); mxcam::Hook_glEndList(); }
MXCAM_EXPORT void glClear(GLbitfield mask) { MXCAM_REAL(glClear); mxcam::Hook_glClear(mask); }
MXCAM_EXPORT void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices)
{
    MXCAM_REAL(glDrawElements);
    mxcam::Hook_glDrawElements(mode, count, type, indices);
}
MXCAM_EXPORT void glDrawArrays(GLenum mode, GLint first, GLsizei count) { MXCAM_REAL(glDrawArrays); mxcam::Hook_glDrawArrays(mode, first, count); }
MXCAM_EXPORT void glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    MXCAM_REAL(glTexParameteri);
    mxcam::Hook_glTexParameteri(target, pname, param);
}
MXCAM_EXPORT void glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{
    MXCAM_REAL(glTexParameterf);
    mxcam::Hook_glTexParameterf(target, pname, param);
}
MXCAM_EXPORT void glTexParameteriv(GLenum target, GLenum pname, const GLint* params)
{
    MXCAM_REAL(glTexParameteriv);
    mxcam::Hook_glTexParameteriv(target, pname, params);
}
MXCAM_EXPORT void glTexParameterfv(GLenum target, GLenum pname, const GLfloat* params)
{
    MXCAM_REAL(glTexParameterfv);
    mxcam::Hook_glTexParameterfv(target, pname, params);
}
MXCAM_EXPORT void glDeleteTextures(GLsizei n, const GLuint* textures) { MXCAM_REAL(glDeleteTextures); mxcam::Hook_glDeleteTextures(n, textures); }
MXCAM_EXPORT void glBindTexture(GLenum target, GLuint texture) { MXCAM_REAL(glBindTexture); mxcam::Hook_glBindTexture(target, texture); }
MXCAM_EXPORT void glDepthMask(GLboolean flag) { MXCAM_REAL(glDepthMask); mxcam::Hook_glDepthMask(flag); }
MXCAM_EXPORT void glEnable(GLenum capability) { MXCAM_REAL(glEnable); mxcam::Hook_glEnable(capability); }
MXCAM_EXPORT void glDisable(GLenum capability) { MXCAM_REAL(glDisable); mxcam::Hook_glDisable(capability); }
MXCAM_EXPORT void glXSwapBuffers(void* display, unsigned long drawable)
{
    MXCAM_REAL(glXSwapBuffers);
    mxcam::Hook_glXSwapBuffers(display, drawable);
}
MXCAM_EXPORT void (*glXGetProcAddress(const GLubyte* name))()
{
    MXCAM_REAL(glXGetProcAddress);
    return mxcam::Hook_glXGetProcAddress(name);
}
MXCAM_EXPORT void (*glXGetProcAddressARB(const GLubyte* name))()
{
    MXCAM_REAL(glXGetProcAddressARB);
    return mxcam::Hook_glXGetProcAddressARB(name);
}
MXCAM_EXPORT unsigned eglSwapBuffers(void* display, void* surface)
{
    MXCAM_REAL(eglSwapBuffers);
    return mxcam::real_eglSwapBuffers ? mxcam::Hook_eglSwapBuffers(display, surface) : 0;
}
MXCAM_EXPORT void (*eglGetProcAddress(const char* name))()
{
    MXCAM_REAL(eglGetProcAddress);
    return mxcam::Hook_eglGetProcAddress(name);
}
#endif
