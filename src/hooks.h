// hooks.h
//   Interface between the generic proxy (proxy.cpp) and the intercepted GL functions (hooks.cpp).
#pragma once

#include <windows.h>
#include <cstddef>

namespace mxcam {

// The genuine system opengl32.dll (loaded by proxy.cpp).
HMODULE RealOpenGLModule();

// A driver entry point that bypasses all of our hooks: opengl32 exports first, then the real
// wglGetProcAddress. Needs a current GL context for anything past OpenGL 1.1.
void* GetRealGLFunction(const char* name);

// True when the game window is the foreground window (hotkeys are ignored otherwise).
bool GameHasFocus();

// Out on a track (riding, spectating, a replay): the game's 3D camera seen in the last half second -
// not in its menus.
bool OnTrack();

struct HookEntry {
    const char* name;      // export name, e.g. "glLoadMatrixf"
    void*       hook;      // our replacement, which the export stub will jump to
    void**      realSlot;  // receives the genuine function from the system opengl32.dll
};

// The table of exports hooks.cpp wants to intercept. The proxy fills every realSlot before
// pointing the export at the hook, so a hook can always call its real function.
const HookEntry* GetHookTable(size_t& count);

// Frame dump, shared with the extension wrappers in proc_hooks.cpp.
bool IsDumping();
void DumpCall(const char* format, ...);

// Called for every function the game fetches through wglGetProcAddress (OpenGL 2.0+ and
// extensions). Returns our wrapper for the ones we intercept, otherwise the real pointer.
void* WrapProc(const char* name, void* real);

// A draw call as the game made it: glDrawElements, glDrawArrays or glDrawRangeElements.
struct DrawCall {
    enum Kind { Elements, Arrays, RangeElements } kind = Elements;
    unsigned    mode    = 0;
    int         first   = 0;        // glDrawArrays
    unsigned    start   = 0;        // glDrawRangeElements
    unsigned    end     = 0;
    int         count   = 0;
    unsigned    type    = 0;        // index type (the Elements kinds)
    const void* indices = nullptr;  // offset into the element buffer
};

// Every draw call, before it is made. The terrain's are recorded so replays can fill in the
// ground the game culled (proc_hooks.cpp). Returns false to leave the draw out (the game's lens
// flare under a time of day of our own).
bool NoteDraw(const DrawCall& draw);

// glBindTexture, as the game calls it (tracked so the shadow casters' textures can be put back
// without asking the driver).
void NoteBindTexture(unsigned int target, unsigned int texture);  // GLenum, GLuint

// glDepthMask and glEnable / glDisable, as the game calls them: see-through draws (dust, roost,
// smoke: blended, or not writing depth) cast no shadow-map shadows.
void NoteDepthMask(bool on);
void NoteCapability(unsigned int capability, bool on);  // GLenum

// The shadow casters held back this frame (static scenery: its geometry, program and textures
// don't change during the frame) are drawn into the shadow map in one go, right before the
// shadows are applied: one switch of framebuffer and state instead of one per object. Called by
// postfx; Discard drops them (a frame that wasn't applied).
void FlushShadowCasters();
// The see-through draws held back this frame, drawn again for their own depth (postfx).
void FlushSeeThroughDepth();
void DiscardShadowCasters();

// Called once per frame by the swap hook. Returns true if the shader path found the camera
// this frame.
bool EndShaderFrame();

// The last frame drew the 3D scene (riding, or a replay), not just the game's menus.
bool SceneOnScreen();

// The game's own menu (Esc) is open over the 3D scene: the HUD and cursor are left alone with it.
bool GameMenuOverScene();

// For the OTHER tab: what hiding the game's HUD and cursor is doing ("" when both are off).
const char* GameOverlayStatusText();

// Writes the last 15 s of per-frame camera data to MXS_REMASTERED_trace.csv, then keeps recording for
// the given number of seconds.
void StartTrace(double seconds);

}  // namespace mxcam
